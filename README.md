# ESP32 BLE Weather Station

A three-board ESP32 weather station, built to answer one question: **how long
can an outdoor sensor run on a 2000 mAh LiPo?**

The original design woke from deep sleep, read its sensor, did a full WiFi +
TLS handshake, published to AWS IoT and went back to sleep. That flattened the
battery in a few days. This version replaces the whole radio side of the cycle
with a BLE advertisement — no association, no handshake, no TLS on the battery
board. Measured deep sleep is **~100 µA** and the whole thing should run for
months rather than days.

## How it works

```
 [peripheral]                     [central]
 outdoors, battery                indoors, mains

 wakes every 10 min               scans continuously
 reads BME280                     decodes the advertisement
 advertises ~0.5 s ───BLE──────>  republishes it as JSON
 deep sleeps                      + publishes its own reading every 10 min
                                        │
                                        │
 [bedroom]                              │   MQTT over TLS
 mains, own sensor ─────────────────────┤   (device certificate)
 (own Thing, cert and own topic)        │
                                        v
                             AWS IoT Core (message broker)
                                        │   IoT rule: from 'home/+/reading'
                                        v
                                   DynamoDB
                                        │
                                        v
                          Lambda (Function URL) ──> dashboard
```

**No device talks to DynamoDB.** Each board holds a certificate whose policy
allows only `iot:Connect` on `client/<board>*` and `iot:Publish` on
`topic/home/<board>/*`. The database write is performed by the IoT rule, under
an IAM role that only the IoT service can assume. A compromised board could
publish bogus readings to its own topic, but it has no way to read or write the
table.

The outdoor reading travels as a **packed binary struct** inside the BLE
advertisement's manufacturer-data field — advertisements only have ~24 usable
bytes, nowhere near enough for JSON. The central board decodes it and
republishes JSON over its own existing MQTT connection, so no new AWS
infrastructure was needed for the outdoor board: it reuses the indoor
certificate and is distinguished by the payload's `board` field.

The bedroom board is independent — own Thing, own certificate, own topic — and
publishes to AWS IoT over its own MQTT connection rather than relaying through
the central board. It exists because the relay path only makes sense for a
board that can't afford a radio of its own.

## Hardware

| Board | Role | Sensor |
|---|---|---|
| Seeed XIAO ESP32-S3 | **central** — indoors, mains. Scans BLE, publishes everything | BME280 |
| Seeed XIAO ESP32-S3 | **peripheral** — outdoors, 2000 mAh LiPo. Advertises and sleeps | BME280 |
| Seeed XIAO ESP32-S3 (Sense) | **bedroom** — mains. Publishes directly | BMP280 (no humidity) |

Sensors are I2C at `0x76` or `0x77`, wired to SDA=GPIO5 / SCL=GPIO6
(silkscreen `D4`/`D5` — those are Arduino pin numbers, not GPIO numbers, which
is an easy and confusing mistake).

All three sketches **auto-detect** which part is fitted, because cheap
breakout modules are frequently mislabelled: one module bought as a BME280
turned out to be a BMP280, and another was dead on arrival. The BME280 driver
refuses a BMP280 outright, so a mismatched board silently reads `NaN` for
everything unless you handle it.

## Repository layout

One PlatformIO project, three environments sharing an `[env]` base section,
with `build_src_filter` selecting which sketch compiles:

```
src/peripheral_main.cpp   [env:peripheral]   outdoor board — advertise and sleep
src/central_main.cpp      [env:central]      indoor board — scan, decode, relay
src/bedroom_main.cpp      [env:bedroom]      bedroom board — publish directly
include/secrets_*.h       WiFi + AWS IoT credentials (gitignored)
summary.md                full write-up: architecture, gotchas, power measurements
```

## Getting started

```bash
pio run -e central -t upload      # or peripheral / bedroom
```

Credentials live in `include/secrets_bedroom.h`, `include/secrets_central.h`
etc. Start from `include/secrets_bedroom.h.example` — it holds the WiFi SSID
and password, the AWS IoT endpoint, and the device certificate/key. Those files
are gitignored; only the example is committed.

**Two flashing gotchas worth knowing:**

- **Always pass an explicit port.** PlatformIO's auto-detection will pick a
  legacy `/dev/ttyS0` when no board is present, and will happily flash the
  wrong board when two are attached. Map ports to boards by USB serial and use
  the stable `by-id` path:

  ```bash
  ls -l /dev/serial/by-id/    # usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00
  pio run -e central -t upload --upload-port /dev/serial/by-id/usb-...-if00
  ```

- **The peripheral needs the BOOT-hold dance**: hold BOOT, plug in USB, release.
  It deep-sleeps about a second after starting, and on the ESP32-S3 the native
  USB peripheral disappears when it sleeps — so the port only exists for a
  couple of seconds and a normal upload can't catch it. Holding BOOT during
  power-up latches ROM download mode, where the application never runs. The
  other two boards never sleep and flash normally.

## Things that cost real time

- **`mqtt.publish(topic, buf, len)` does not do what it looks like.** A `char*`
  will not convert to `const uint8_t*`, so the call silently binds to
  `publish(topic, payload, retained)` with `retained = (len != 0)` — publishing
  every message retained. Pass the payload as `(const uint8_t*)` explicitly.
- **`getEfuseMac()` packs the *first* MAC octet in the low byte.** Deriving a
  unique client ID from the low 24 bits gives you the Espressif OUI, which is
  identical on every board from the same block — so two boards got the same
  "unique" ID. The device-specific octets are bits 24-47.
- **A shared MQTT client ID is a reconnect storm, not a failure to connect.**
  AWS accepts the new connection and disconnects the already-connected client,
  so both boards evict each other every few seconds forever, while each still
  publishes successfully in the gaps. Symptom: `MQTT connected.` repeating with
  a rock-steady hold time.
- **An IoT rule's `SELECT` is a filter, not a projection.** Any field the
  firmware publishes but the rule doesn't name is dropped before DynamoDB, with
  no error anywhere.
- **A BMP280 that reads plausibly can still be wrong.** One module sat 9 hPa
  below two BME280s that agreed with each other to within 0.6 hPa. 9 hPa is
  ~75 m of altitude, so it wasn't a location difference.
- **Loose I2C wiring was the most common failure by far** — `chip ID 0x00` at
  both addresses with `ESP_ERR_TIMEOUT`. Use soldered or crimped joints for
  anything that lives outdoors.
- **On the XIAO, early boot serial output is routinely lost** before the native
  USB CDC port starts streaming, so anything printed in the first seconds of
  `setup()` may never reach the monitor.

## Full write-up

See [`summary.md`](summary.md) for the complete picture: the AWS/CDK backend,
the BLE payload format in detail, the battery budget with measured numbers, and
the longer list of things that went wrong.
