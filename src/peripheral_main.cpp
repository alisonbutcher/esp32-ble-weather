#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

// --- Shared payload format ---
// Must match the struct in central_main.cpp exactly - this is a plain
// packed binary blob carried inside the BLE advertisement's manufacturer
// data, not JSON (BLE advertisements only have ~24-26 usable bytes, far too
// tight for a JSON string).
//
// Fixed-point encoding (x10) avoids needing floats over the wire, at one
// decimal place of precision - plenty for a weather reading:
//   temp_c_x10      : 263   -> 26.3 C
//   humidity_x10    : 641   -> 64.1 %
//   pressure_hpa_x10: 10179 -> 1017.9 hPa
//   sensor_type     : 1 = BME280, 2 = BMP280, 0 = unknown
//
// humidity_pct_x10 == 0xFFFF is a sentinel meaning "this part has no humidity
// element" (a BMP280). The fixed-point format has no other way to express
// that, and 0 is a legitimate reading - 0 % - that must not be confused with
// "absent".
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

// 0xFFFF is the Bluetooth SIG's reserved "for testing only" company
// identifier - fine for this bench experiment, would need a registered
// company ID (or a different advertisement structure) for a real product.
#define TEST_COMPANY_ID 0xFFFF

#define SLEEP_SECONDS (10UL * 60) // 10 min production interval (was 30s for debugging)
// How long to advertise before sleeping again. This window is the single
// biggest load on the battery: measured burst current is ~130 mA, so every
// millisecond removed here is ~0.2 uA off the average, and dropping from
// 2000 ms to 500 ms takes roughly 0.33 mA off it. At the 100-200 ms
// advertising interval below, 500 ms still puts 2-5 advertisements in the
// air, and the central scans continuously with duplicate filtering off, so it
// only has to catch one. If this ever proves too short it shows up as gaps in
// the outdoor series rather than failing silently.
#define ADVERTISE_DURATION_MS 500

Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
NimBLEAdvertising* advertising;

enum SensorKind : uint8_t { SENSOR_NONE, SENSOR_BME280, SENSOR_BMP280 };
SensorKind sensorKind = SENSOR_NONE;

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
// check decide. 0x60 = BME280, 0x58 = BMP280. Without this a BMP280 would
// fail bme.begin() and then encode NaN through (uint16_t)(humidity * 10),
// which is undefined behaviour - and if it landed on 0, that reads as 0 %
// humidity, which is inside central's plausibility bounds and would be stored
// as a real reading.
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
    return true;
  }
  for (uint8_t i = 0; i < 2; i++) {
    Serial.printf("  probe 0x%02X: chip ID 0x%02X\n", addrs[i], readChipIdAt(addrs[i]));
  }
  return false;
}

// Forced mode: exactly one measurement per wake, then the part goes back to
// sleep on its own.
//
// Adafruit's begin() calls setSampling() with MODE_NORMAL and a 0.5 ms standby,
// which makes the sensor re-measure roughly continuously (~350 uA) - and it
// keeps doing that through the ESP32's deep sleep, because nothing powers the
// sensor rail down between cycles. At a ~0.3 mA average budget that single
// line is the largest load on the battery. Forced mode drops idle draw to
// well under 1 uA.
void configureForcedMode() {
  if (sensorKind == SENSOR_BME280) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X16,   // temp
                    Adafruit_BME280::SAMPLING_X16,   // pressure
                    Adafruit_BME280::SAMPLING_X16,   // humidity
                    Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5);
  } else if (sensorKind == SENSOR_BMP280) {
    // Note the BMP280 library's standby enum starts at STANDBY_MS_1, unlike
    // the BME280's STANDBY_MS_0_5. It only matters in normal mode.
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
                    Adafruit_BMP280::SAMPLING_X16,   // temp
                    Adafruit_BMP280::SAMPLING_X16,   // pressure
                    Adafruit_BMP280::FILTER_OFF,
                    Adafruit_BMP280::STANDBY_MS_1);
  }
}

void deepSleepAndRestart() {
  Serial.println("Sleeping...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  Serial.println("BLE peripheral waking...");

  Wire.begin(5, 6);
  if (!initSensor()) {
    // Nothing readable on the bus - better to advertise nothing than to send a
    // fabricated reading, so sleep and retry next cycle.
    Serial.println("No supported sensor found, sleeping and retrying next cycle.");
    deepSleepAndRestart();
  }
  configureForcedMode();

  float temp, humidity, pressure;
  if (sensorKind == SENSOR_BME280) {
    bme.takeForcedMeasurement();
    temp = bme.readTemperature();
    humidity = bme.readHumidity();
    pressure = bme.readPressure() / 100.0F;
  } else {
    bmp.takeForcedMeasurement();
    temp = bmp.readTemperature();
    pressure = bmp.readPressure() / 100.0F;
    humidity = NAN; // BMP280 has no humidity element
  }
  Serial.printf("%s: temp=%.1f C, humidity=%.1f %%, pressure=%.1f hPa\n",
                sensorName(), temp, humidity, pressure);

  SensorPayload payload;
  payload.temp_c_x10 = (int16_t)(temp * 10);
  payload.humidity_pct_x10 = (sensorKind == SENSOR_BME280)
                               ? (uint16_t)(humidity * 10)
                               : HUMIDITY_ABSENT;
  payload.pressure_hpa_x10 = (uint16_t)(pressure * 10);
  payload.sensor_type = (sensorKind == SENSOR_BME280) ? SENSOR_TYPE_BME280
                       : (sensorKind == SENSOR_BMP280) ? SENSOR_TYPE_BMP280
                                                       : SENSOR_TYPE_UNKNOWN;

  std::string mfgData;
  mfgData += (char)(TEST_COMPANY_ID & 0xFF);
  mfgData += (char)((TEST_COMPANY_ID >> 8) & 0xFF);
  mfgData.append((char*)&payload, sizeof(payload));

  NimBLEDevice::init("WeatherBLE");
  advertising = NimBLEDevice::getAdvertising();
  advertising->setMinInterval(160); // 100ms, in 0.625ms units
  advertising->setMaxInterval(320); // 200ms

  NimBLEAdvertisementData advData;
  advData.setName("WeatherBLE");
  advData.setManufacturerData(mfgData);
  advertising->setAdvertisementData(advData);

  advertising->start();
  Serial.println("Advertising...");

  delay(ADVERTISE_DURATION_MS); // give the central board time to catch this advertisement

  advertising->stop();
  NimBLEDevice::deinit(true); // fully release the BLE stack before sleeping
  deepSleepAndRestart();
}

void loop() {} // never reached; deep sleep resets back to setup()




