# ESP32 Weather Station — Project Summary

Three related codebases: the original WiFi/MQTT station (`esp32-weather`), the
AWS backend (`weather-infra`), and this BLE-relay rewrite
(`esp32-ble-weather`), built to solve the outdoor board's short battery life.

## Hardware

- **3x Seeed XIAO ESP32-S3** — one is the Sense variant (camera/mic, not used
  for anything camera-related), two are plain. All three are now in service:
  - **central** — indoors, mains/USB powered, does the BLE scanning and all
    MQTT publishing
  - **peripheral** — outdoors, battery only, BLE advertising
  - **bedroom** — mains/USB powered, own sensor, own MQTT connection
- **Sensors**, I2C at `0x76` (SDO low) or `0x77` (SDO high), wired to
  SDA=GPIO5, SCL=GPIO6 (silkscreen `D4`/`D5` — the `D` labels are Arduino pin
  numbers, not GPIO numbers, easy to mix up):
  - central: BME280 (chip ID `0x60`) — has humidity
  - peripheral: BME280 (chip ID `0x60`) — has humidity
  - bedroom: **BMP280** (chip ID `0x58`) — no humidity element, reports null.
    Planned to be swapped for a BME280.
  - One module bought as a "BME280" turned out to be a BMP280 and was
    dead-on-arrival (no I2C ACK at either address, wiring verified). Cheap
    modules are frequently mislabelled in both directions.
- **XIAO Debug Mate** — bought expecting DAPLink chip debugging; DAPLink does
  **not** support any ESP32 variant (Xtensa/RISC-V, not ARM Cortex-M), so
  breakpoint debugging is unusable. The **power profiler and serial monitor
  work fine**, and the profiler is the only reason we have real current
  numbers (see "Measured power" below).
- Battery: 2000 mAh LiPo on the peripheral, soldered to the XIAO's battery
  pads. A 2000 mAh cell on WiFi-only firmware lasted only a few days — that
  is what triggered the whole BLE rework.

## Original project: `esp32-weather` (WiFi + MQTT)

**Toolchain**: PlatformIO in VS Code (moved off Arduino IDE). Arduino
framework, not ESP-IDF.

**Firmware pattern** (both indoor and outdoor originally): wake from deep
sleep → read sensor → connect WiFi → connect MQTT/TLS → publish JSON →
disconnect → deep sleep. `SLEEP_MINUTES` controls the cycle (10 min in
production, was set to 1 for testing).

**Known WiFi gotchas hit along the way**:
- XIAO's native USB serial drops on every reset/sleep wake — the monitor
  needs to reattach, and **early boot output is routinely lost** before the
  port starts streaming. The `while (!Serial && millis()-start<3000)` pattern
  only partly mitigates it; anything printed in the first few seconds of
  `setup()` (including sensor detection lines) may never reach the monitor.
  Practical workaround: re-print important diagnostics on a later code path
  that runs once the port is definitely up.
- The home router runs WPA2/WPA3 mixed mode, which the
  ESP32 Arduino WiFi stack has known bugs negotiating (`WL_DISCONNECTED`,
  status 6; `AUTH_FAIL(202)` then `AUTH_EXPIRE(2)` before succeeding). Fixed
  by forcing WPA2-PSK explicitly via `esp_wifi_set_config` +
  `pmf_cfg.required = false`, bypassing `WiFi.begin()`, rather than changing
  the router's security mode. Expect the first association after a boot to
  still fail once and succeed on retry.
- **Suspected cause of the original short battery life**: the shutdown
  sequence only called `WiFi.disconnect(true)`, not `esp_wifi_stop()` +
  `WiFi.mode(WIFI_OFF)` + `btStop()`. Missing the explicit radio power-down
  before `esp_deep_sleep_start()` is a well-documented cause of much higher
  than expected sleep current. Now partly vindicated: the BLE peripheral
  sleeps at ~100 µA (see "Measured power"), so a properly torn-down radio is
  not leaving a large residual load.

## AWS backend

**Originally built manually via AWS CLI in `us-east-1`**, then migrated to
**Infrastructure as Code (AWS CDK, TypeScript) in `ap-southeast-2`** — the
`weather-infra` project. Two stacks.

### `WeatherInfraStack` (ap-southeast-2)
- DynamoDB table `WeatherReadings` — partition key `board` (string), sort key
  `timestamp` (number). Provisioned 5/5 RCU/WCU to stay inside the Always Free
  tier. TTL attribute defined (`ttl`) but not yet populated — planned tiered
  retention (full-res 30 days, hourly averages after, TTL-delete at 12 months)
  is designed but not built.
- **One IoT policy per board** (`weather-indoor-policy`, `weather-outdoor-policy`,
  `weather-bedroom-policy`), each:
  - `iot:Connect` on `client/<board>*` — the trailing wildcard exists so each
    device can append a unique suffix derived from its MAC. Without it, two
    boards flashed with the same sketch present the same client ID and AWS
    disconnects the already-connected one **in a loop, forever**.
  - `iot:Publish` + `iot:RetainPublish` on `topic/home/<board>/*`.
    **Critical gotcha**: publishing with the MQTT retain flag requires
    `iot:RetainPublish` in addition to `iot:Publish` — omitting it causes a
    silent `AUTHORIZATION_FAILURE` that `aws iot test-authorization` does
    **not** catch (the simulator says ALLOWED while the real broker denies).
    Cost a long debugging session, found via CloudWatch IoT logs
    (`aws iot set-v2-logging-options` + tailing `AWSIotLogsV2`).
    **Note**: the firmware no longer publishes retained at all (see the
    `publish()` overload trap below), so this permission is now belt-and-braces
    rather than load-bearing.
- IoT Rule `WeatherReadingsToDynamoDB`:
  ```
  SELECT board, online, rssi, sensor_type, temperature_c, humidity_pct,
         pressure_hpa, timestamp() as timestamp FROM 'home/+/reading'
  ```
  Routes all boards' topics into DynamoDB via `dynamoDBv2`/`putItem` — no
  Lambda needed for plain storage. **The `SELECT` is a filter, not a
  projection**: any field the firmware publishes but the rule does not name is
  silently dropped before DynamoDB. This bit us twice — `humidity` vs
  `humidity_pct` (bedroom humidity never stored) and `sensor_type`/`online`/
  `rssi` being absent for a while.
- Lambda `WeatherDashboardAPI` (**Node 24.x**; was Node 20.x, which AWS
  deprecated 2026-04-30) — reads DynamoDB (last N hours, all boards) and
  returns items **verbatim**, so any new attribute flows to the dashboard with
  no Lambda change. Exposed via a **Lambda Function URL** (not API Gateway —
  simpler, still free-tier friendly). CORS locked to
  `https://weather.alisonbutcher.com`. **Gotcha hit**: the Lambda was also
  setting `Access-Control-Allow-Origin` in its own response headers *in
  addition* to the Function URL's CORS config; browsers reject the duplicate
  header. Fix: let the Function URL's CORS config be the only source.
  `BOARDS` is a hardcoded list in the Lambda — a new board must be added there
  or the API will not return it.
- Device provisioning (Things + X.509 certs) is **not** in the CDK stack —
  CloudFormation can't auto-generate keypairs the way the console's
  "auto-generate certificate" button does. `scripts/provision-device.sh`
  handles it as a one-time step per device, after the stack has created the
  policy it attaches to.

### `WeatherDashboardHostingStack` (us-east-1 — hard CloudFront/ACM requirement)
- S3 bucket (private, `BLOCK_ALL`), CloudFront via Origin Access Control, ACM
  certificate for `weather.alisonbutcher.com` (DNS-validated against the
  existing Route53 zone), Route53 alias record.
- `BucketDeployment` auto-invalidates CloudFront on every deploy — no manual
  cache-busting.
- Dashboard: single static `dashboard/index.html`, no build step. IBM Plex
  Mono/Sans, dark instrument-panel style (amber=indoor, blue=outdoor,
  teal=bedroom). Shows current temp/humidity/pressure plus three trend charts
  with a 6h/24h/3d/7d toggle. Chart.js + chartjs-adapter-date-fns loaded via
  **jsDelivr, not cdnjs** (cdnjs doesn't reliably mirror the date-adapter
  package; also hit a bug where a made-up Chart.js version, 4.4.4, doesn't
  exist as a release — always verify CDN version numbers).
  - Each station shows its **`sensor_type`** next to the timestamp.
  - The outdoor `{"online": false}` stale marker renders as **"No reading for
    X min"** rather than three em dashes that look like a broken sensor. The
    age is computed from the timestamp, so the unselected
    `minutes_since_last_reading` field turned out not to be needed.
  - Pressure is charted for **indoor only** (`BOARDS.filter(b => b.pressure)`)
    — the bedrooms and outdoor deliberately don't publish pressure, since it
    barely varies room-to-room. This accidentally insulated the dashboard from
    a badly-calibrated BMP280 (see below).
- No forecast integration yet (planned: Open-Meteo, free, no API key).

**Data migration**: old `us-east-1` DynamoDB data was copied to the new
`ap-southeast-2` table with a small Python script shelling out to the `aws`
CLI (not boto3 directly — boto3 hit a `MissingDependencyException` tied to
this account's credential setup that the CLI doesn't have; never diagnosed).

**Region migration note**: MQTT endpoint changed
(`*.iot.ap-southeast-2.amazonaws.com`), and each device needed a **new Thing +
new certificate** — certs are tied to the account/region's IoT Core instance
and don't carry over. A custom MQTT domain (`mqtt.alisonbutcher.com`) was
considered and rejected: real setup cost for no gain.

### Old `us-east-1` setup (retired)

The original manual CLI build lived in `us-east-1` and was left running after
the migration. It was fully decommissioned once the region move was trusted.
Removed: the old IoT rule, two **still-ACTIVE** device certificates (live
credentials for a dead endpoint, with an *enabled* rule behind them — the
actual reason this was worth doing), two Things, three policies (one of them a
`weather-indoor-test-wildcard` test artefact), the orphaned
`IoTToDynamoDBRole`, the old `WeatherReadings` table and the `AWSIotLogsV2`
log group. The table's 281 items were dumped to
`weather-infra/weather-readings-us-east-1-backup.json` first, since deleting a
table is irreversible.

**Kept deliberately**: `IoTLoggingRole`, which is *shared* — both regions point
at it for IoT v2 logging, so deleting it would have broken logging in the live
region. Also kept: both CDK stacks, the CDK bootstrap asset buckets, and the
`weather-outdoor` partition in the live table.

Two gotchas from the teardown, in case it's ever repeated: `list-principal-things`
and `list-thing-principals` are inverses but only the latter returned the
certificate↔Thing attachment (`DeleteCertificate` refuses while a Thing is
attached), and a policy with more than one version cannot be deleted until
every non-default version is removed with `delete-policy-version`.

## `esp32-ble-weather` (BLE relay)

Built to test whether BLE advertising (instead of a full WiFi+TLS handshake
every cycle) meaningfully improves the outdoor board's battery life.

### Architecture

The **peripheral** (outdoor, battery) wakes every 10 minutes, reads its
BME280, advertises the reading for a fraction of a second, and sleeps. The
**central** (indoor, mains) scans continuously, decodes the advertisement and
republishes it to AWS IoT over its own WiFi/MQTT connection, alongside
publishing its own indoor reading on a separate 10-minute timer. The
**bedroom** board is independent: own sensor, own MQTT connection, own topic.

No dedicated AWS infrastructure is needed for the outdoor data. The central
reuses the `weather-indoor` certificate; the outdoor reading goes to
`home/weather-indoor/reading` and is distinguished by the JSON `board` field
(`weather-indoor` vs `weather-outdoor`). A single shared identity was a
deliberate choice: the outdoor sensor's only path to AWS is through the central
board anyway, so a separate certificate would add complexity for no isolation
benefit. The bedroom board has its own Thing/certificate and its own topic
(`home/weather-bedroom/reading`), since it has its own MQTT connection to AWS
IoT rather than relaying through the central board. It still never touches
DynamoDB: it publishes to its topic and the IoT rule performs the write.

### Wire payload

BLE advertisements only have ~24-26 usable bytes, far too tight for JSON, so
the reading travels as a packed binary struct in the manufacturer-data field,
tagged with company ID `0xFFFF` (the Bluetooth SIG's reserved "testing only"
ID — fine for a personal project, would need a registered ID for a product):

```cpp
struct __attribute__((packed)) SensorPayload {
  int16_t  temp_c_x10;        // 263   -> 26.3 C
  uint16_t humidity_pct_x10;  // 641   -> 64.1 %
  uint16_t pressure_hpa_x10;  // 10179 -> 1017.9 hPa
  uint8_t  sensor_type;       // 1 = BME280, 2 = BMP280, 0 = unknown
};
```

**Must be byte-identical in `peripheral_main.cpp` and `central_main.cpp`.**

Two things about the encoding that are easy to get wrong:

- `humidity_pct_x10 == 0xFFFF` is a **sentinel** meaning "this part has no
  humidity element". Fixed-point has no other way to express that, and `0` is
  a legitimate reading (0 %) that must not be conflated with "absent". The
  decoder must test the sentinel **before** the plausibility bounds, or 0xFFFF
  decodes as 6553.5 % and the whole reading is discarded.
- The field is **appended last**, so the two mismatched-firmware cases are not
  symmetric. New peripheral + old central: the size check still passes and the
  first six bytes decode correctly, so only `sensor_type` is lost (and the
  sentinel trips). Old peripheral + new central: the size check fails and
  **every** reading is dropped. Hence: **flash the peripheral first.**

### Project layout

One PlatformIO project, three environments sharing an `[env]` base section,
with `build_src_filter` picking which `.cpp` compiles per environment:

- `[env:peripheral]` → `src/peripheral_main.cpp` (battery outdoor board)
- `[env:central]` → `src/central_main.cpp` (indoor board)
- `[env:bedroom]` → `src/bedroom_main.cpp` (bedroom board)

The `central`, `peripheral` and `bedroom` environments each add the Adafruit
BMP280 library alongside the BME280 one, because all three do dual-sensor
detection.

### Sensor detection (all three boards)

All three sketches probe for both parts and drive whichever is actually
fitted. The pattern, and why it matters:

- **Drivers first, not a raw probe.** Try `bme.begin(addr)` then
  `bmp.begin(addr)` and let each library's own chip-ID check decide. An earlier
  version read register `0xD0` by hand *first* and used that to choose a
  driver — which meant a quirk in that hand-rolled I2C transaction could veto a
  perfectly good sensor. The raw probe is now used **only for reporting** when
  both drivers decline.
- **Both strap addresses** — `0x76` and `0x77`. `Adafruit_BMP280`'s default
  address is `0x77`, so a module strapped high is invisible to a hardcoded
  `0x76`.
- **Refuse to publish rather than fabricate.** On `SENSOR_NONE` the central and
  bedroom boards skip the publish entirely (a row of nulls is worse than no
  row), and the peripheral skips advertising and goes straight back to sleep.
- **Why it matters here specifically**: `Adafruit_BME280::init()` hard-requires
  chip ID `0x60` and returns *before* reading the calibration on anything else.
  On a BMP280 that leaves `_measReg`/`_humReg` zeroed, so every read returns
  `NAN`. On the peripheral that `NAN` used to go through
  `(uint16_t)(humidity * 10)` — undefined behaviour — and if it landed on `0`,
  that reads as 0 % humidity, which is **inside** the central's plausibility
  bounds and would have been stored as a real reading.

### Peripheral firmware

Wake → detect sensor → forced-mode read → build payload → advertise → stop →
`NimBLEDevice::deinit(true)` (properly release the BLE radio before sleeping,
same principle as the WiFi `esp_wifi_stop()` fix) → deep sleep.

- `SLEEP_SECONDS = 10 * 60` — a 10-minute cycle.
- `ADVERTISE_DURATION_MS = 500`. **This window is the single biggest load on
  the battery** — at a measured ~130 mA burst, each millisecond costs ~0.22 µA
  of average current. It was reduced from 2000 ms on that basis. The risk is
  that the central's BLE and WiFi share one radio, so a short burst can fall
  entirely inside a WiFi/TLS window and be missed; 500 ms still emits 2-5
  advertisements at the configured 100-200 ms interval, and the central only
  needs to catch one. If the outdoor series ever looks gappy with the central
  healthy, **1000 ms is the fallback**.
- **Forced-mode sampling is deliberate.** `bme.begin()` inherits Adafruit's
  defaults of `MODE_NORMAL` with `STANDBY_MS_0_5`, which makes the sensor
  re-measure roughly continuously and keeps doing so *through* the ESP32's deep
  sleep, because nothing powers the sensor rail down between cycles. The sketch
  calls `setSampling(MODE_FORCED, ...)` and `takeForcedMeasurement()` so the
  part takes one measurement and parks itself at well under 1 µA.
  **Caveat**: the BMP280 library's standby enum starts at `STANDBY_MS_1`, not
  the BME280's `STANDBY_MS_0_5` — a straight copy-paste doesn't compile.

### Central firmware

Connects WiFi + MQTT once at boot (it can't deep sleep — it has to keep
scanning), then loops: scan continuously for the peripheral's advertisement →
publish the decoded reading immediately as `"board":"weather-outdoor"` → every
10 minutes, read its own BME280 and publish `"board":"weather-indoor"`.

- **Scan settings matter**: `setActiveScan(false)` (the peripheral puts
  everything in the primary advertisement, so there's nothing to answer), and
  `setDuplicateFilter(false)` — NimBLE's controller-level duplicate filtering
  would otherwise report the peripheral's first advertisement ever and silently
  drop every subsequent one, since it reuses the same BLE address.
  `setMaxResults(0)` puts the scan in callback-only mode so it can run forever.
- **Offline marker**: if no BLE reading has arrived for 40 minutes
  (`OUTDOOR_STALE_THRESHOLD_MS`, ~4 missed cycles), it publishes
  `{"board":"weather-outdoor","online":false,...}` once, so a dead or
  out-of-range peripheral is distinguishable from the central itself being
  down. The dashboard renders this as "No reading for X min".
- **Known issue, unresolved**: the central hung silently once, for 28 minutes,
  with the board powered throughout, and recovered on a power cycle. It was
  noticeably warm while hung. A `heap=` / `maxblk=` / `uptime=` line now prints
  with each 10-minute publish so the next occurrence can be classified: a
  downward heap trend means memory exhaustion, a flat line means a deadlock
  (and would point at the NimBLE host task rather than the heap). Capture it
  with `pio device monitor --filter log2file`.

### Unique MQTT client IDs

Each board derives its client ID from its own MAC:

```cpp
uint64_t mac = ESP.getEfuseMac();   // NOTE: first octet in the LOW byte
snprintf(id, sizeof(id), "weather-indoor-%02X%02X%02X",
         (mac >> 40) & 0xFF, (mac >> 32) & 0xFF, (mac >> 24) & 0xFF);
```

**The byte order is a trap.** `getEfuseMac()` packs the MAC with the *first*
octet in the low byte, so the low 24 bits are the Espressif **OUI** — identical
on every board from the same block. An earlier version used those bytes and
produced the *same* suffix (`BD4D74`) on two different boards, which is exactly
the collision the unique ID exists to prevent. Use the top three octets (bits
24-47). Current IDs: `weather-indoor-80B995`, `weather-bedroom-74B895`.

The identity was made per-device after a long evening lost to two boards both
running the same sketch and evicting each other every ~5 seconds
(`DUPLICATE_CLIENTID`). **The topic and the JSON `board` field stay role-based;
only the client ID is per-device.**

### Measured power (Debug Mate)

The Debug Mate's power meter is specified for **1 µA to 1 A, ±1 % from 10 µA**
— genuinely capable of the µA range, unlike most hobby profilers. It measures
the **5 V input** to the XIAO, so it includes the LDO's losses and reads
*higher* than the actual battery draw: treat it as a safe upper bound.

| Measurement | Value |
|---|---|
| Deep sleep, normal-mode sampling | ~130 µA |
| Deep sleep, forced-mode sampling | ~100 µA |
| Awake peak (BLE TX, CPU at 240 MHz) | ~660 mW ≈ 132 mA |

The awake peak is what matters, because the duty cycle is tiny but the burst is
enormous relative to sleep:

```
burst    132 mA x ~2.8 s / 600 s  ≈ 0.60 mA average   (2 s window)
sleep                             ≈ 0.10 mA
                                  ─────────
total                             ≈ 0.70 mA  -> ~4 months on 2000 mAh
```

With the 500 ms window the burst term drops to ~0.28 mA, giving **~0.38 mA and
roughly 7 months**. Derate for ~85 % usable capacity before cutoff and LiPo
self-discharge (~2-3 %/month).

Two takeaways worth remembering: the forced-mode change bought far less than
the datasheet reasoning predicted (the sensor was never the dominant load), and
**the advertising window is where the energy actually is**. A precise average
was never captured — the Debug Mate's UI 3 accumulates total charge (Ah) and
time (`average = Total Ah / Time`), which is the right tool for a 0.45 % duty
cycle, but the measurement was abandoned as not worth further bench time.

**Sense caveat**: Seeed quote the XIAO ESP32-S3 Sense at **3 mA** deep sleep
against the plain board's 14 µA. That column is measured *with the camera/SD
expansion board attached* (adjacent rows say so explicitly, and the +17 mA
modem-sleep delta only makes sense with it fitted). Without the daughterboard
expect tens of µA. Never use a Sense for a battery node with the carrier
attached.

### Flashing

**Always pass an explicit port.** With no board present, PlatformIO
"auto-detects" `/dev/ttyS0` and fails confusingly; with two boards attached it
may flash the wrong one (this happened — the bedroom board spent ~10 minutes
running the central sketch).

```bash
ls -l /dev/serial/by-id/    # map USB serial -> ttyACMx
pio run -e central -t upload \
  --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_74:4D:BD:95:B9:80-if00
```

Port **indices shift on every reset**; the `by-id` serial is stable.

**The peripheral needs the BOOT-hold dance** (hold BOOT, plug in USB, release):
it deep-sleeps ~1.3 s after starting, and on the ESP32-S3 the native USB
peripheral goes away when it sleeps — so the port only exists for a couple of
seconds and PlatformIO can't upload. Holding BOOT during power-up latches ROM
download mode, where the application never runs and the port stays up. Central
and bedroom never sleep, so they flash normally.

### Hardware gotchas

- **Loose I2C connections were the most common failure by far** — twice the
  symptom was `chip ID 0x00` at both addresses with
  `[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error 263`
  (`ESP_ERR_TIMEOUT` = no ACK). A breadboard is fine on the bench; the outdoor
  box needs soldered or crimped joints, because a marginal contact there
  produces silent data gaps that look exactly like a dead peripheral.
- **A bad BMP280 reads plausibly.** One module sits **9 hPa below** two
  BME280s that agree with each other to within 0.6 hPa, measured minutes apart.
  9 hPa is ~75 m of altitude, so it can't be a location difference: that
  module's pressure channel is out of calibration. Temperature was only ~1 °C
  off, which is within normal room-to-room variation. Don't use it as a
  pressure reference.
- **Battery charging from solar** (designed, not built): a solar cell is a
  current-limited source, and an ordinary LiPo charger tries to pull a constant
  current, so the panel voltage collapses below the charger's UVLO and it
  hiccups. Needs a blocking diode, some form of MPPT, and — most importantly —
  a charging path whose *quiescent* current is well under the ~0.3 mA load.
  The load is ~27 mWh/day, so a palm-sized panel is 10-150x oversized and the
  panel is not the hard part. Note that flashing requires USB and USB charges
  the battery, so a clean battery-current measurement and a flash are mutually
  exclusive states.

## Outstanding / next steps

- **Swap the bedroom sensor for a BME280.** It currently runs a BMP280, so it
  has no humidity. The code needs no change — detection picks the driver.
- **Outdoor enclosure/weatherproofing**: sensor in a louvered
  shield with airflow (a sunlit box reads several degrees high, and the ESP32's
  own duty cycle adds heat), panel and sensor placed separately since they want
  opposite things from the sun, soldered joints. Charging a LiPo below ~0 °C
  damages it, and many charger ICs have an NTC input that silently inhibits
  charging if no thermistor is fitted.
- **If the outdoor series turns gappy with the central healthy, raise
  `ADVERTISE_DURATION_MS` to 1000.**
- **Watch the `rssi` values** now recorded on outdoor rows (> −70 dBm strong,
  −70 to −85 workable, below −85 marginal) — it's the early warning for a
  marginal placement.
- **Chase the central's 28-minute hang** if it recurs, using the heap line.
- Dashboard: add forecast (Open-Meteo, free/no API key); optionally display
  `rssi`.
- DynamoDB TTL + downsampling Lambda (12-month auto-purge, tiered resolution) —
  designed conceptually, not built.
- Optionally drop IoT Core v2 logging from `DEBUG` to `ERROR` in both regions —
  it's still on from the original debugging session. Harmless at this message
  volume, just noisy.
- **LoRa experiment** (for fun, not need): a second node somewhere genuinely
  remote. LoRa suits this better than BLE — range aside, the 2-second
  advertising window that costs ~82 % of the BLE energy budget is replaced by a
  ~50 ms transmit, and LoRa payloads are large enough that the packed struct
  hack disappears. In Australia use **AU915** (915-928 MHz, LIPD class
  licence), not US915/EU868. Antenna height matters far more than the antenna
  itself; a λ/4 at 915 MHz is only ~8 cm. Public network options are The Things
  Network (community gateways, free, but its fair-access policy allows only
  ~30 s of uplink airtime per device per day — a 10-minute cadence needs a low
  spreading factor and a compact payload) and commercial operators NNNCo and
  Meshed. Helium coverage in AU/NZ is degraded; not recommended.
