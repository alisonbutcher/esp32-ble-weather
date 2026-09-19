#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

#include "secrets_bedroom.h"

#define BOARD_ROLE "weather-bedroom"
#define MQTT_PORT 8883
#define WIFI_TIMEOUT_MS 15000
#define PUBLISH_INTERVAL_MS (10UL * 60 * 1000)  // own reading, every 10 min
#define PUBLISH_RETRY_INTERVAL_MS (60UL * 1000) // retry a failed publish rather than lose the reading
#define MQTT_RETRY_INTERVAL_MS (5UL * 1000)     // don't hammer a broker that's rejecting/unreachable
#define WIFI_RETRY_INTERVAL_MS (60UL * 1000)    // retry WiFi ourselves rather than rely on the core's auto-reconnect

// This board has been fitted with a BMP280 and a BME280 at different times,
// and a module sold as a BME280 turned out to be another BMP280. The two parts
// are pin- and register-compatible for temperature and pressure but report
// different chip IDs (0x58 vs 0x60), and the BME280 driver refuses anything
// that isn't 0x60 - it returns before reading the calibration, which leaves
// every reading NaN. So probe the chip ID and drive whichever part is really
// fitted. Only the BME280 has a humidity element.
Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
WiFiClientSecure net;
PubSubClient mqtt(net);

unsigned long lastPublish = 0;                       // millis() of the last publish attempt
unsigned long publishInterval = PUBLISH_INTERVAL_MS; // 10 min, or a short retry after a failure
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastConnectedAt = 0; // DIAG: millis() of the last successful MQTT connect

// The MQTT client ID has to be unique per *device*, not per role. Two boards
// flashed with this sketch once shared BOARD_ROLE as their client ID and spent
// an evening evicting each other, because AWS disconnects the already-connected
// client when a duplicate ID appears. The publish topic and the JSON "board"
// field stay as BOARD_ROLE - only the client ID carries the suffix. This needs
// the IoT policy's iot:Connect resource widened to client/weather-bedroom*,
// otherwise CONNECT comes back as state 5 (not authorized).
char mqttClientId[48] = {0};

void buildClientId() {
  // getEfuseMac() packs the MAC with the first octet in the LOW byte, so the
  // low 24 bits are the Espressif OUI - identical on every board from that
  // block. The device-specific part is the top three octets (bits 24-47).
  // Using the low bytes produced the same suffix on two different boards,
  // which is exactly the collision this is supposed to prevent.
  uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttClientId, sizeof(mqttClientId), "%s-%02X%02X%02X",
           BOARD_ROLE,
           (unsigned)((mac >> 40) & 0xFF),
           (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 24) & 0xFF));
}

enum SensorKind : uint8_t { SENSOR_NONE, SENSOR_BME280, SENSOR_BMP280 };

// Detection result, stored so it can be re-reported later. The copy setup()
// prints is routinely lost - on the XIAO's native USB CDC the port isn't
// streaming yet when setup() runs, so the first few lines of boot output
// never reach the monitor.
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
// check decide. 0x60 = BME280 (has humidity), 0x58 = BMP280 (no humidity).
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

// DIAG - makes WiFi link events visible. The sketch's own connectWiFi() only
// prints when it actually runs, which is up to 60s after a drop and only once
// the retry gate has passed, so a brief flap that recovers on its own would
// otherwise leave no trace in the log at all.
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    // Reason 0 upsets the core's name lookup, so normalise it the same way
    // WiFiGeneric.cpp does.
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (!reason) reason = WIFI_REASON_UNSPECIFIED;
    Serial.printf("WiFi event: STA_DISCONNECTED reason=%s(%u) t=%lu\n",
                  WiFi.disconnectReasonName((wifi_err_reason_t)reason),
                  (unsigned)reason, millis());
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.printf("WiFi event: STA_CONNECTED t=%lu\n", millis());
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("WiFi event: STA_GOT_IP t=%lu\n", millis());
  }
}

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

  // DIAG: the client ID is what AWS IoT names in its lifecycle events, and
  // heap/maxblk show whether repeated TLS handshakes are leaking or
  // fragmenting memory - maxblk (largest contiguous block) is what a new TLS
  // session actually needs, so it can fail while free heap still looks OK.
  Serial.printf("Connecting to AWS IoT as client '%s'... t=%lu heap=%u maxblk=%u\n",
                mqttClientId, millis(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (mqtt.connect(mqttClientId)) {
    lastConnectedAt = millis();
    Serial.printf("MQTT connected. t=%lu heap=%u maxblk=%u\n",
                  lastConnectedAt, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  } else {
    Serial.printf("MQTT connect failed, state: %d t=%lu heap=%u maxblk=%u\n",
                  mqtt.state(), millis(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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
  bool ok = mqtt.publish("home/" BOARD_ROLE "/reading", (const uint8_t*)buf, len);
  Serial.printf("Publish: %s\n", ok ? "success" : "failed");

  for (int i = 0; i < 10; i++) {
    mqtt.loop();
    delay(20);
  }
  return ok;
}

// Returns false only when a reading could not be delivered, so the caller can
// retry sooner than a full interval.
bool publishReading() {
  if (sensorKind == SENSOR_NONE) {
    // Nothing to read - publishing nothing beats a row of nulls. Report what
    // was on the bus too, since this is the line that survives to the monitor.
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
    humidity = NAN;  // BMP280 has no humidity element - serialises as JSON null
  }

  Serial.printf("Bedroom reading (%s): temp=%.1fC  humidity=%.1f%% pressure=%.1fhPa\n",
                sensorName(), temp, humidity, pressure);
  // Repeated here because the copy printed in setup() is usually lost before
  // the USB CDC port starts streaming.
  Serial.printf("Sensor status: address 0x%02X, chip ID 0x%02X, driver %s\n",
                (unsigned)sensorAddr, (unsigned)sensorChipId, sensorName());

  JsonDocument doc;
  doc["board"] = BOARD_ROLE;
  doc["online"] = true;
  // Which part is actually fitted. Note the IoT rule only forwards the fields
  // it names in its SELECT, so this needs adding there too or it is dropped
  // before DynamoDB - same trap as humidity vs humidity_pct.
  doc["sensor_type"] = sensorName();
  doc["temperature_c"] = temp;
  // humidity_pct, not humidity: the IoT rule's SELECT is board, temperature_c,
  // humidity_pct, pressure_hpa, timestamp() - a "humidity" key is silently
  // dropped and never reaches DynamoDB.
  doc["humidity_pct"] = humidity;
  // No pressure_hpa either - only central's indoor reading is kept as the
  // household's one pressure source, since it barely varies room-to-room.
  return publishRaw(doc);
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  Serial.println("Bedroom board starting...");

  buildClientId();

  Wire.begin(5, 6);
  if (!initSensor()) {
    Serial.printf("No supported sensor at 0x76 or 0x77 (best chip ID 0x%02X) - readings will be skipped.\n",
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

  // DIAG only - see onWiFiEvent(). Registered before the first association so
  // nothing is missed.
  WiFi.onEvent(onWiFiEvent);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    connectMQTT();
  }

  lastPublish = millis() - publishInterval; // publish immediately at boot
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

  // DIAG: report the instant the session is lost, how long it survived
  // ("held"), PubSubClient's state code, and whether the WiFi link was still
  // up at that moment. state -3 = socket closed underneath us (peer hung up,
  // or the local stack dropped it); -4 = PubSubClient's own keepalive gave up.
  // "held" is the number that settles the ~5s vs ~27s vs ~100s question.
  static bool wasConnected = false;
  bool nowConnected = mqtt.connected();
  if (wasConnected && !nowConnected) {
    Serial.printf("MQTT dropped t=%lu state=%d held=%lums heap=%u maxblk=%u wifi=%d rssi=%d\n",
                  millis(), mqtt.state(), millis() - lastConnectedAt,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  (int)WiFi.status(), WiFi.RSSI());
  }
  wasConnected = nowConnected;

  // Only restart the full interval once a publish actually succeeds. This used
  // to advance lastPublish before the attempt, so a tick that landed while the
  // link was down silently lost that reading for a whole interval.
  if (millis() - lastPublish > publishInterval) {
    lastPublish = millis();
    publishInterval = publishReading() ? PUBLISH_INTERVAL_MS
                                       : PUBLISH_RETRY_INTERVAL_MS;
  }

  delay(200);
}
