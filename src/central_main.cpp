#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

#include "secrets_central.h"

#define MQTT_PORT 8883
#define WIFI_TIMEOUT_MS 15000
#define TEST_COMPANY_ID 0xFFFF
#define PERIPHERAL_NAME "WeatherBLE"
#define OWN_PUBLISH_INTERVAL_MS (10UL * 60 * 1000)       // this board's own indoor reading, every 10 min
#define OWN_PUBLISH_RETRY_INTERVAL_MS (60UL * 1000)      // retry a failed publish rather than lose the reading
#define OUTDOOR_STALE_THRESHOLD_MS (40UL * 60 * 1000)    // no BLE reading in 40 min (~4 missed 10-min cycles) -> report offline
#define MQTT_RETRY_INTERVAL_MS (5UL * 1000)              // don't hammer a broker that's rejecting/unreachable
#define WIFI_RETRY_INTERVAL_MS (60UL * 1000)              // retry WiFi ourselves rather than rely on the core's auto-reconnect
#define OUTDOOR_DEBOUNCE_MS (5UL * 1000)                 // peripheral repeats the same reading every ~100-200ms for its ~2s burst; only forward the first packet of each burst

// Physically-plausible bounds for the decoded outdoor reading. Anything
// outside this is either a corrupted payload or (since TEST_COMPANY_ID
// 0xFFFF is the Bluetooth SIG's public "for testing only" ID) a stray
// advertisement from an unrelated nearby device that happens to collide
// on company ID - discard rather than publish it as real weather data.
#define MIN_PLAUSIBLE_TEMP_C -40.0f
#define MAX_PLAUSIBLE_TEMP_C 85.0f
#define MIN_PLAUSIBLE_HUMIDITY_PCT 0.0f
#define MAX_PLAUSIBLE_HUMIDITY_PCT 100.0f
#define MIN_PLAUSIBLE_PRESSURE_HPA 300.0f
#define MAX_PLAUSIBLE_PRESSURE_HPA 1100.0f

// Must match peripheral_main.cpp exactly, including the sensor_type byte and
// the humidity sentinel below.
#define HUMIDITY_ABSENT 0xFFFF
#define SENSOR_TYPE_UNKNOWN 0
#define SENSOR_TYPE_BME280 1
#define SENSOR_TYPE_BMP280 2

struct __attribute__((packed)) SensorPayload {
  int16_t temp_c_x10;
  uint16_t humidity_pct_x10;
  uint16_t pressure_hpa_x10;
  uint8_t sensor_type;
};

const char* payloadSensorName(uint8_t sensorType) {
  switch (sensorType) {
    case SENSOR_TYPE_BME280: return "BME280";
    case SENSOR_TYPE_BMP280: return "BMP280";
    default:                 return "unknown";
  }
}

// This board has only ever had a BME280, but the household's sensors have been
// shuffled between boards, and a BMP280 here would fail bme.begin() and then
// quietly publish a row of nulls every 10 minutes. Detect which part is really
// fitted, exactly as the bedroom board does. Only the BME280 has a humidity
// element; a BMP280 reports humidity as null.
Adafruit_BME280 bme;
Adafruit_BMP280 bmp;

enum SensorKind : uint8_t { SENSOR_NONE, SENSOR_BME280, SENSOR_BMP280 };
SensorKind sensorKind = SENSOR_NONE;
uint8_t sensorChipId = 0;
uint8_t sensorAddr = 0;

const char* sensorName() {
  switch (sensorKind) {
    case SENSOR_BME280: return "BME280";
    case SENSOR_BMP280: return "BMP280";
    default:            return "none";
  }
}

// Both parts put the chip ID in register 0xD0. Used only for reporting, so a
// quirk in this hand-rolled transaction can never veto a working sensor.
uint8_t readChipIdAt(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0xD0);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint16_t)addr, (uint8_t)1) != 1) return 0;
  return (uint8_t)Wire.read();
}

// Drivers first, not probe first: try both parts at both addresses they strap
// to (SDO low = 0x76, SDO high = 0x77) and let each library's own chip-ID
// check decide. 0x60 = BME280, 0x58 = BMP280.
bool initSensor() {
  const uint8_t addrs[] = {0x76, 0x77};
  for (uint8_t i = 0; i < 2; i++) {
    if (bme.begin(addrs[i])) {
      sensorKind = SENSOR_BME280;
    } else if (bmp.begin(addrs[i])) {
      sensorKind = SENSOR_BMP280;
    } else {
      continue;
    }
    sensorAddr = addrs[i];
    sensorChipId = (sensorKind == SENSOR_BME280) ? (uint8_t)bme.sensorID()
                                                : bmp.sensorID();
    return true;
  }

  // Neither driver claimed it - probe both addresses purely so the log says
  // what was actually there (0x00 = nothing answered).
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t id = readChipIdAt(addrs[i]);
    if (id) {
      sensorAddr = addrs[i];
      sensorChipId = id;
      break;
    }
  }
  return false;
}

WiFiClientSecure net;
PubSubClient mqtt(net);
NimBLEScan* scan;

// Written from the NimBLE host task (ScanCallbacks::onResult), read/cleared
// from loop(). Guarded by readingMux so a reading is always handed over whole
// rather than torn between two different advertisements.
portMUX_TYPE readingMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool newOutdoorReading = false;
SensorPayload latestOutdoorReading;
int lastRssi = 0;

// Only touched from onResult, which NimBLE only ever calls from its own host
// task one event at a time - no locking needed for this one.
unsigned long lastOutdoorAccept = 0;

unsigned long lastOwnPublish = 0;                            // millis() of the last own-reading publish attempt
unsigned long ownPublishInterval = OWN_PUBLISH_INTERVAL_MS;   // 10 min, or a short retry after a failure
unsigned long lastOutdoorSeen = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
bool outdoorOfflineReported = false;

// Unique per-device MQTT client ID. A shared ID meant two boards flashed with
// the same sketch evicted each other forever, because AWS disconnects the
// already-connected client when a duplicate ID appears. The publish topic and
// the JSON "board" field are unchanged - only the identity is unique. This
// needs the IoT policy's iot:Connect resource to allow client/weather-indoor*.
char mqttClientId[48] = {0};

void buildClientId() {
  // getEfuseMac() packs the MAC with the first octet in the LOW byte, so the
  // low 24 bits are the Espressif OUI - identical on every board from that
  // block. The device-specific part is the top three octets (bits 24-47).
  // Using the low bytes produced the same suffix on two different boards,
  // which is exactly the collision this is supposed to prevent.
  uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttClientId, sizeof(mqttClientId), "weather-indoor-%02X%02X%02X",
           (unsigned)((mac >> 40) & 0xFF),
           (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 24) & 0xFF));
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) {
    if (!device->haveName() || device->getName() != PERIPHERAL_NAME) return;
    if (!device->haveManufacturerData()) return;

    std::string mfgData = device->getManufacturerData();
    if (mfgData.size() < 2 + sizeof(SensorPayload)) return;

    uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);
    if (companyId != TEST_COMPANY_ID) return;

    SensorPayload payload;
    memcpy(&payload, mfgData.data() + 2, sizeof(payload));

    float temp = payload.temp_c_x10 / 10.0f;
    float pressure = payload.pressure_hpa_x10 / 10.0f;
    // 0xFFFF means "no humidity element fitted" (a BMP280 outdoors). Test it
    // before the bounds check, since it would otherwise decode as 6553.5 % and
    // get the whole reading thrown away.
    bool humidityAbsent = (payload.humidity_pct_x10 == HUMIDITY_ABSENT);
    float humidity = humidityAbsent ? NAN : payload.humidity_pct_x10 / 10.0f;

    if (temp < MIN_PLAUSIBLE_TEMP_C || temp > MAX_PLAUSIBLE_TEMP_C ||
        (!humidityAbsent && (humidity < MIN_PLAUSIBLE_HUMIDITY_PCT ||
                             humidity > MAX_PLAUSIBLE_HUMIDITY_PCT)) ||
        pressure < MIN_PLAUSIBLE_PRESSURE_HPA || pressure > MAX_PLAUSIBLE_PRESSURE_HPA) {
      Serial.println("Discarding implausible outdoor reading.");
      return;
    }

    int rssi = device->getRSSI();
    Serial.printf("BLE recv (%s): temp=%.1fC humidity=%.1f%% pressure=%.1fhPa rssi=%d\n",
                  payloadSensorName(payload.sensor_type), temp, humidity, pressure, rssi);

    // The peripheral repeats this same reading every ~100-200ms for its whole
    // advertise burst - only forward the first packet of each burst so we
    // don't publish (and re-write to DynamoDB) the same reading several
    // times a cycle.
    if (millis() - lastOutdoorAccept < OUTDOOR_DEBOUNCE_MS) return;
    lastOutdoorAccept = millis();

    portENTER_CRITICAL(&readingMux);
    latestOutdoorReading = payload;
    lastRssi = rssi;
    newOutdoorReading = true;
    portEXIT_CRITICAL(&readingMux);
  }
};

// Wired up only because NimBLEScan::onHostSync() uses a non-null
// scanCompleteCB as its signal to auto-restart a duration==0 (continuous)
// scan after a BLE host reset/resync - it's never otherwise invoked, since a
// continuous scan has no normal completion.
void onScanEnd(NimBLEScanResults results) {}

void connectWiFi() {
  lastWifiAttempt = millis();
  WiFi.mode(WIFI_STA);

  wifi_config_t conf = {};
  strncpy((char*)conf.sta.ssid, WIFI_SSID, sizeof(conf.sta.ssid) - 1);
  strncpy((char*)conf.sta.password, WIFI_PASSWORD, sizeof(conf.sta.password) - 1);
  conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;

  esp_wifi_set_config(WIFI_IF_STA, &conf);
  esp_wifi_connect();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi connect failed.");
  }
}

void connectMQTT() {
  lastMqttAttempt = millis();
  mqtt.setServer(MQTT_ENDPOINT, MQTT_PORT);

  Serial.println("Connecting to AWS IoT...");
  // Print the real client ID: it is the quickest way to tell which build a
  // board is running, and a shared client ID is what once had two boards
  // evicting each other all evening.
  Serial.printf("MQTT client ID: %s\n", mqttClientId);
  if (mqtt.connect(mqttClientId)) {
    Serial.println("MQTT connected.");
  } else {
    Serial.printf("MQTT connect failed, state: %d\n", mqtt.state());
  }
}

bool publishRaw(JsonDocument& doc) {
  char buf[256];
  size_t len = serializeJson(doc, buf, sizeof(buf));

  Serial.print("Publishing: ");
  Serial.println(buf);

  // Explicit-length overload on purpose: the (topic, char*, size_t) form
  // silently binds to publish(topic, payload, retained) with
  // retained = (len != 0), because a char* will not convert to const uint8_t*.
  // That made every publish retained and dependent on iot:RetainPublish.
  bool ok = mqtt.publish("home/weather-indoor/reading", (const uint8_t*)buf, len);
  Serial.printf("Publish: %s\n", ok ? "success" : "failed");

  for (int i = 0; i < 10; i++) {
    mqtt.loop();
    delay(20);
  }
  return ok;
}

// Returns false only when the reading could not be delivered, so loop() can
// retry sooner than a full interval.
bool publishOwnReading() {
  if (sensorKind == SENSOR_NONE) {
    // Nothing to read - publishing nothing beats a row of nulls.
    Serial.printf("No sensor detected (probed 0x76 and 0x77, best chip ID 0x%02X) - skipping publish.\n",
                  (unsigned)sensorChipId);
    return true; // nothing to deliver, so there is nothing to retry
  }

  float temp, humidity, pressure;
  if (sensorKind == SENSOR_BME280) {
    temp = bme.readTemperature();
    humidity = bme.readHumidity();
    pressure = bme.readPressure() / 100.0F;
  } else {
    temp = bmp.readTemperature();
    pressure = bmp.readPressure() / 100.0F;
    humidity = NAN; // BMP280 has no humidity element - serialises as JSON null
  }

  Serial.printf("Indoor reading (%s): temp=%.1fC humidity=%.1f%% pressure=%.1fhPa\n",
                sensorName(), temp, humidity, pressure);
  // DIAG: heap trend, sampled once per publish (every 10 min). A steady decline
  // over hours points at the 28-minute silent hang being memory exhaustion; a
  // flat line right up to a hang would point at a deadlock instead.
  Serial.printf("heap=%u maxblk=%u uptime=%lus\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), millis() / 1000);

  JsonDocument doc;
  doc["board"] = "weather-indoor";
  doc["online"] = true;
  doc["sensor_type"] = sensorName();
  doc["temperature_c"] = temp;
  doc["humidity_pct"] = humidity;
  doc["pressure_hpa"] = pressure;
  return publishRaw(doc);
}

void publishOutdoorReading(const SensorPayload& payload, int rssi) {
  float temp = payload.temp_c_x10 / 10.0;
  // NAN serialises as JSON null, which is how "no humidity element" should
  // appear in DynamoDB rather than a fabricated 0 %.
  float humidity = (payload.humidity_pct_x10 == HUMIDITY_ABSENT)
                     ? NAN
                     : payload.humidity_pct_x10 / 10.0;

  // Pressure is still decoded and plausibility-checked in onResult() (it's
  // part of the same BLE payload either way), but not published - it barely
  // varies room-to-room/outdoor-to-indoor the way temp and humidity do, so
  // only central's own indoor reading is kept as the household's one
  // pressure source rather than three near-identical lines.
  JsonDocument doc;
  doc["board"] = "weather-outdoor";
  doc["online"] = true;
  doc["sensor_type"] = payloadSensorName(payload.sensor_type);
  doc["temperature_c"] = temp;
  doc["humidity_pct"] = humidity;
  doc["rssi"] = rssi;
  publishRaw(doc);
}

void publishOutdoorOffline() {
  unsigned long minutesSinceLastSeen = (millis() - lastOutdoorSeen) / 60000;

  JsonDocument doc;
  doc["board"] = "weather-outdoor";
  doc["online"] = false;
  doc["minutes_since_last_reading"] = minutesSinceLastSeen;
  publishRaw(doc);
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  Serial.println("Indoor board + BLE relay starting...");

  buildClientId();

  Wire.begin(5, 6);
  if (!initSensor()) {
    Serial.printf("No supported sensor at 0x76 or 0x77 (best chip ID 0x%02X) - own readings will be skipped.\n",
                  (unsigned)sensorChipId);
  } else {
    Serial.printf("Sensor at 0x%02X: chip ID 0x%02X (%s)\n",
                  (unsigned)sensorAddr, (unsigned)sensorChipId, sensorName());
  }

  // Set once here rather than on every MQTT reconnect attempt - these don't
  // change at runtime.
  net.setCACert(AWS_ROOT_CA);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(DEVICE_PRIVATE_KEY);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    connectMQTT();
  }

  NimBLEDevice::init("");
  scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setInterval(100);
  scan->setWindow(99);

  // This board is mains-powered and has no reason to pause listening the way
  // the battery peripheral does - scan continuously and non-blockingly for
  // the whole lifetime of the sketch instead of cycling discrete scans from
  // loop(). Passive (not active) scanning is deliberate: the peripheral puts
  // everything it sends in the primary advertisement payload (no separate
  // scan-response data), and active scanning's callback delivery for a
  // continuous (duration=0) scan otherwise depends on a scan-response
  // round-trip that has nothing to answer it here. setMaxResults(0) puts the
  // scan in callback-only mode so it can run forever without accumulating
  // results in memory. setDuplicateFilter(false) matters a lot here: NimBLE's
  // discovery procedure defaults to controller-level duplicate filtering
  // ("ignore all but the first advertisement from each device"), which the
  // old discrete scan cycles reset every ~6s just by restarting. A single
  // continuous session never restarts, so without this the controller would
  // report the peripheral's very first advertisement ever and then silently
  // drop every one after it, since it keeps reusing the same BLE address.
  scan->setActiveScan(false);
  scan->setMaxResults(0);
  scan->setDuplicateFilter(false);
  scan->start(0, onScanEnd, false);

  lastOwnPublish = millis() - ownPublishInterval; // publish immediately at boot
  lastOutdoorSeen = millis();
}

void loop() {
  // Don't rely on the ESP32 core's own STA_DISCONNECTED auto-reconnect - in
  // practice it isn't reliably bringing WiFi back once the first connection
  // attempt fails, so retry it explicitly ourselves instead.
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
    connectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED && !mqtt.connected() &&
      millis() - lastMqttAttempt > MQTT_RETRY_INTERVAL_MS) {
    connectMQTT();
  }
  mqtt.loop();

  portENTER_CRITICAL(&readingMux);
  bool hasReading = newOutdoorReading;
  SensorPayload reading = latestOutdoorReading;
  int rssi = lastRssi;
  newOutdoorReading = false;
  portEXIT_CRITICAL(&readingMux);

  if (hasReading) {
    lastOutdoorSeen = millis();
    outdoorOfflineReported = false;
    publishOutdoorReading(reading, rssi);
  } else if (!outdoorOfflineReported && millis() - lastOutdoorSeen > OUTDOOR_STALE_THRESHOLD_MS) {
    outdoorOfflineReported = true;
    publishOutdoorOffline();
  }

  // Only restart the full interval once the publish actually succeeds, so a
  // tick that lands while the link is down is retried rather than losing the
  // reading for a whole interval.
  if (millis() - lastOwnPublish > ownPublishInterval) {
    lastOwnPublish = millis();
    ownPublishInterval = publishOwnReading() ? OWN_PUBLISH_INTERVAL_MS
                                             : OWN_PUBLISH_RETRY_INTERVAL_MS;
  }

  // Not the "sleep" removed above - BLE scanning above runs continuously on
  // the NimBLE host task regardless of what loop() does. This is just a
  // short yield so loop() (now just polling timestamps/mqtt.loop()) doesn't
  // spin at 100% CPU and starve the watchdog/other FreeRTOS tasks.
  delay(200);
}
