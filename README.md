# SHT41 BTHome Beacon

An ultra-low-power temperature and humidity beacon for the nRF52840, built to run for a long time on a single CR2032 coin cell. It reads an SHT41 sensor once a minute and broadcasts the values as a [BTHome v2](https://bthome.io/) Bluetooth LE advertisement, so Home Assistant (and any other BTHome-compatible receiver) picks it up automatically with no pairing or connection.

## Features

- Temperature (0.1 °C resolution), relative humidity (1 %) and battery voltage (1 mV) in a standard BTHome v2 packet
- Sensor is power-gated from a GPIO and powered only for ~15 ms per measurement
- Non-connectable advertising every 10 s, measurement every 60 s
- DC/DC converters enabled on both regulator stages (REG0 and REG1)
- CPU sleeps between events via FreeRTOS tickless idle
- Bare-metal TWIM (I²C) and SAADC drivers that are enabled only while in use
- Battery measured internally through VDDH/5, so no external voltage divider is needed
- SHT41 readings are CRC-checked; on a failed read the last good values keep being advertised

## Hardware

| Part | Notes |
|---|---|
| Pro Micro nRF52840 (nice!nano v2 compatible) | Uses the Adafruit UF2 bootloader + S140 SoftDevice |
| Adafruit SHT41 breakout (or bare SHT41) | I²C address `0x44` |
| CR2032 coin cell | Connected to the board's battery input |

### Wiring

| SHT41 | nRF52840 GPIO | Pro Micro label |
|---|---|---|
| VIN | P0.17 | D2 / `017` |
| SDA | P0.20 | D3 / `020` |
| SCL | P0.22 | D4 / `022` |
| GND | GND | GND |

The sensor's VIN comes from a GPIO, not from the 3V3 rail, which is what allows it to be switched off completely between measurements. Pins are configured as raw nRF GPIO numbers (`port * 32 + pin`) and driven directly through registers, so the Arduino/Feather pin mapping is irrelevant. Internal pull-ups are enabled on SDA/SCL as a backup, but the breakout's own pull-ups are what the bus normally relies on.

## Building and flashing

The project uses PlatformIO with the Adafruit nRF52 Arduino core. The Feather nRF52840 board definition is used because it shares the same bootloader and SoftDevice as the nice!nano.

Expected layout:

```
.
├── platformio.ini
└── src/
    └── main.ino
```

Build and upload:

```bash
pio run                 # build
pio run -t upload       # flash over USB (double-tap reset to enter the bootloader if needed)
```

Flash over USB **without** the coin cell inserted, or make sure your board's power path can handle both sources at once.

If you prefer the Arduino IDE, install the Adafruit nRF52 core and select **Tools → Board → Adafruit Feather nRF52840 Express**.

## Configuration

All user settings are `#define`s at the top of `main.ino`:

| Setting | Default | Description |
|---|---|---|
| `SENS_PWR_PIN` | `17` | GPIO powering the sensor |
| `SENS_SDA_PIN` / `SENS_SCL_PIN` | `20` / `22` | I²C pins |
| `MEAS_INTERVAL_MS` | `60000` | Time between measurements (ms) |
| `ADV_INTERVAL` | `16000` | Advertising interval in 0.625 ms units (16000 = 10 s) |
| `TX_POWER_DBM` | `0` | TX power: -40, -20, -16, -12, -8, -4, 0, 2…8 dBm. Lower saves power but reduces range |
| `DEVICE_NAME` | `"SHT41"` | Advertised name |

The device name is placed in the same 31-byte advertising packet as the BTHome data, so keep it to 13 characters or fewer.

## BTHome payload

Service data under UUID `0xFCD2`, device info byte `0x40` (BTHome v2, unencrypted, regular interval), followed by objects in ascending ID order:

| Object ID | Measurement | Type | Factor |
|---|---|---|---|
| `0x0C` | Voltage | uint16 | 0.001 V |
| `0x2E` | Humidity | uint8 | 1 % |
| `0x45` | Temperature | sint16 | 0.1 °C |

## Home Assistant

With the BTHome integration and a Bluetooth adapter or ESPHome Bluetooth proxy in range, the beacon is discovered automatically within a few seconds of powering up. Accept the discovery to get temperature, humidity and voltage entities. Values update once a minute.

## How it works

On boot the firmware enables the DC/DC converters, sets the sensor pins to high-impedance, preconfigures the TWIM peripheral, starts the SoftDevice and begins advertising. Every `MEAS_INTERVAL_MS` it then:

1. Powers the SHT41 and switches SDA/SCL to I²C mode
2. Sends the high-precision measurement command (`0xFD`), sleeps ~10 ms, and reads 6 bytes
3. Disables TWIM, sets the bus pins to high-Z and then cuts sensor power (in that order, to avoid back-feeding the sensor through the I/O lines)
4. Verifies both CRCs and converts the raw values
5. Samples VDDH/5 with the SAADC right after the sensor load, then turns the ADC off
6. Rebuilds the advertisement with the new values

Between measurements the CPU stays in System ON sleep while the SoftDevice handles advertising.

## Notes

- Serial is intentionally never started; USB is unpowered on battery anyway.
- The status LED blink during advertising is disabled.
- The first measurement after boot is published even if it fails (as zeros), so the device is visible right away; later failures keep the previous values.
