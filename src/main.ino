/*
  BTHome v2 temperature / humidity beacon  –  ultra-low-power
  ------------------------------------------------------------
  Board  : Pro Micro nRF52840 (nice!nano v2 compatible), powered by CR2032
  Sensor : Adafruit SHT41 (I2C addr 0x44), power-gated from a GPIO
  Core   : Adafruit nRF52 Arduino core
           Tools > Board > "Adafruit Feather nRF52840 Express"
           (same UF2 bootloader + S140 SoftDevice as the nice!nano)

  Behaviour
   - Measures T/RH + battery voltage every 60 s
   - Sensor is powered ONLY during the measurement (~15 ms)
   - Non-connectable BTHome v2 advertisement every 10 s
   - DC/DC enabled on REG0 (VDDH->VDD) and REG1 (VDD->1.3 V core)
   - CPU sleeps (FreeRTOS tickless idle -> sd_app_evt_wait) the rest of the time

  All pins below are RAW nRF GPIO numbers (port * 32 + pin), driven directly
  through registers, so the Feather pin mapping does not matter.
*/

#include <bluefruit.h>

// ---------------- user settings ----------------
#define SENS_PWR_PIN      17      // P0.17  (Pro Micro "D2" / "017") -> SHT41 VIN
#define SENS_SDA_PIN      20      // P0.20  (Pro Micro "D3" / "020") -> SHT41 SDA
#define SENS_SCL_PIN      22      // P0.22  (Pro Micro "D4" / "022") -> SHT41 SCL

#define MEAS_INTERVAL_MS  60000UL // measurement period
#define ADV_INTERVAL      16000    // 10000 ms / 0.625 ms
#define TX_POWER_DBM      0       // -40,-20,-16,-12,-8,-4,0,2..8  (lower = less power, less range)
#define DEVICE_NAME       "SHT41"

#define SHT41_ADDR        0x44
#define SHT41_CMD_MEAS_HP 0xFD    // high precision, no heater (max 8.3 ms)

// ---------------- state ----------------
static int16_t  g_temp10   = 0;   // °C * 10  (BTHome 0x45)
static uint8_t  g_rh       = 0;   // %RH, 1 % (BTHome 0x2E)
static uint16_t g_battMv   = 0;   // mV       (BTHome 0x0C)
static uint8_t  g_i2cBuf[6];      // EasyDMA buffer (must be RAM): TX = cmd byte, RX = 6 data bytes

// =====================================================================
//  Helpers
// =====================================================================
#define PIN_CNF(dir, in, pull, drive)                       \
  ((GPIO_PIN_CNF_DIR_##dir     << GPIO_PIN_CNF_DIR_Pos)   | \
   (GPIO_PIN_CNF_INPUT_##in    << GPIO_PIN_CNF_INPUT_Pos) | \
   (GPIO_PIN_CNF_PULL_##pull   << GPIO_PIN_CNF_PULL_Pos)  | \
   (GPIO_PIN_CNF_DRIVE_##drive << GPIO_PIN_CNF_DRIVE_Pos))
#define CNF_HIGHZ PIN_CNF(Input,  Disconnect, Disabled, S0S1)  // no input buffer, no pull: ~0 leakage
#define CNF_I2C   PIN_CNF(Input,  Connect,    Pullup,   S0D1)  // open-drain + internal pull-up (backup)
#define CNF_PWR   PIN_CNF(Output, Disconnect, Disabled, H0H1)  // high-drive push-pull

static NRF_GPIO_Type* port(uint32_t pin)       { return (pin & 32) ? NRF_P1 : NRF_P0; }
static void pinCfg(uint32_t pin, uint32_t cnf) { port(pin)->PIN_CNF[pin & 31] = cnf; }
static void busPins(uint32_t cnf)              { pinCfg(SENS_SDA_PIN, cnf); pinCfg(SENS_SCL_PIN, cnf); }

static void sensorPower(bool on) {               // set level first, then make it an output (no glitch)
  NRF_GPIO_Type* p = port(SENS_PWR_PIN);
  (on ? p->OUTSET : p->OUTCLR) = 1UL << (SENS_PWR_PIN & 31);
  pinCfg(SENS_PWR_PIN, CNF_PWR);
}

static void sensorOff() {                        // bus high-Z BEFORE removing power (no back-feeding)
  busPins(CNF_HIGHZ);
  sensorPower(false);                            // held low = sensor fully off
}

// Clear event, fire task, wait (with timeout) for the event
static void trigger(volatile uint32_t& task, volatile uint32_t& evt, uint32_t timeoutMs) {
  evt  = 0;
  task = 1;
  for (uint32_t t0 = millis(); !evt && millis() - t0 < timeoutMs; ) {}
}

// =====================================================================
//  Minimal TWIM (I2C master) – enabled only during a measurement
// =====================================================================
static bool twimRun(volatile uint32_t& task, uint32_t shorts) {
  NRF_TWIM0->EVENTS_STOPPED = 0;
  NRF_TWIM0->EVENTS_ERROR   = 0;
  NRF_TWIM0->SHORTS         = shorts;
  task = 1;

  uint32_t t0 = millis();
  while (!NRF_TWIM0->EVENTS_STOPPED && !NRF_TWIM0->EVENTS_ERROR && millis() - t0 <= 20) {}

  bool ok = NRF_TWIM0->EVENTS_STOPPED && !NRF_TWIM0->EVENTS_ERROR;
  if (!ok) {                                     // NACK / bus error / timeout: force STOP
    trigger(NRF_TWIM0->TASKS_STOP, NRF_TWIM0->EVENTS_STOPPED, 5);
    NRF_TWIM0->ERRORSRC     = NRF_TWIM0->ERRORSRC;   // write-1-to-clear
    NRF_TWIM0->EVENTS_ERROR = 0;
  }
  NRF_TWIM0->EVENTS_STOPPED = 0;
  return ok;
}

static uint8_t shtCrc(const uint8_t* d) {        // CRC-8, poly 0x31, init 0xFF
  uint8_t crc = 0xFF;
  for (int i = 0; i < 2; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

// =====================================================================
//  SHT41 measurement with power gating (updates g_temp10 / g_rh on success)
// =====================================================================
static bool readSHT41() {
  uint8_t* b = g_i2cBuf;

  sensorPower(true);
  busPins(CNF_I2C);
  delay(3);                                      // SHT41 power-up <1 ms + breakout caps
  NRF_TWIM0->ENABLE = TWIM_ENABLE_ENABLE_Enabled;

  b[0] = SHT41_CMD_MEAS_HP;
  bool ok = twimRun(NRF_TWIM0->TASKS_STARTTX, TWIM_SHORTS_LASTTX_STOP_Msk);
  if (ok) {
    delay(10);                                   // CPU sleeps during conversion
    ok = twimRun(NRF_TWIM0->TASKS_STARTRX, TWIM_SHORTS_LASTRX_STOP_Msk) &&
         shtCrc(b) == b[2] && shtCrc(b + 3) == b[5];
  }

  NRF_TWIM0->ENABLE = TWIM_ENABLE_ENABLE_Disabled;
  sensorOff();                                   // power down before doing the math
  if (!ok) return false;

  // T = -45 + 175*raw/65535 (0.1 °C units), RH = -6 + 125*raw/65535 (1 %), one rounding step
  uint32_t rawT  = (b[0] << 8) | b[1];
  uint32_t rawRH = (b[3] << 8) | b[4];
  g_temp10 = -450 + (int32_t)((1750UL * rawT  + 32767UL) / 65535UL);
  int32_t rh =  -6 + (int32_t)((125UL  * rawRH + 32767UL) / 65535UL);
  g_rh = constrain(rh, 0, 100);
  return true;
}

// =====================================================================
//  Battery voltage: VDDH/5 via SAADC (no external divider needed)
// =====================================================================
static uint16_t readBatteryMv() {
  static volatile int16_t result;
  static bool calibrated = false;

  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Over8x;
  NRF_SAADC->CH[0].CONFIG =                      // + internal 0.6 V ref, single-ended, no R (all 0)
      (SAADC_CH_CONFIG_GAIN_Gain1_2  << SAADC_CH_CONFIG_GAIN_Pos) |    // range 0..1.2 V
      (SAADC_CH_CONFIG_TACQ_10us     << SAADC_CH_CONFIG_TACQ_Pos) |
      (SAADC_CH_CONFIG_BURST_Enabled << SAADC_CH_CONFIG_BURST_Pos);
  NRF_SAADC->CH[0].PSELP   = SAADC_CH_PSELP_PSELP_VDDHDIV5;
  NRF_SAADC->CH[0].PSELN   = SAADC_CH_PSELN_PSELN_NC;
  NRF_SAADC->RESULT.PTR    = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;

  if (!calibrated) {
    trigger(NRF_SAADC->TASKS_CALIBRATEOFFSET, NRF_SAADC->EVENTS_CALIBRATEDONE, 10);
    calibrated = true;
  }
  trigger(NRF_SAADC->TASKS_START,  NRF_SAADC->EVENTS_STARTED, 5);
  trigger(NRF_SAADC->TASKS_SAMPLE, NRF_SAADC->EVENTS_END,     5);
  trigger(NRF_SAADC->TASKS_STOP,   NRF_SAADC->EVENTS_STOPPED, 5);
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;   // ADC off between readings

  int32_t raw = result;
  return raw < 0 ? 0 : raw * 6000L / 4096L;      // raw/4096 * 1.2 V * 5
}

// =====================================================================
//  BTHome v2 advertisement (objects in ascending ID order: 0x0C, 0x2E, 0x45)
// =====================================================================
static void updateAdvertising() {
  const uint8_t sd[] = {
    0xD2, 0xFC,                                              // UUID 0xFCD2 (little endian)
    0x40,                                                    // BTHome v2, unencrypted, regular interval
    0x0C, uint8_t(g_battMv), uint8_t(g_battMv >> 8),         // voltage, 1 mV
    0x2E, g_rh,                                              // humidity, 1 %, uint8
    0x45, uint8_t(g_temp10), uint8_t(uint16_t(g_temp10) >> 8) // temperature, 0.1 °C, sint16
  };
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, sd, sizeof(sd));
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.start(0);                // advertise forever
}

// force = publish even if the measurement failed (used once at boot)
static void measureAndPublish(bool force = false) {
  if (readSHT41()) {
    g_battMv = readBatteryMv();                  // read right after sensor load
  } else if (!force) {
    return;                                      // keep advertising last good values
  }
  updateAdvertising();
}

// =====================================================================
void setup() {
  // No Serial.begin(): USB stays unused (it is unpowered on battery anyway)

  // DC/DC on both regulators – write directly while SoftDevice is still off
  uint8_t sdOn = 0;
  sd_softdevice_is_enabled(&sdOn);
  if (!sdOn) {
    NRF_POWER->DCDCEN0 = 1;                      // REG0: VDDH -> VDD
    NRF_POWER->DCDCEN  = 1;                      // REG1: VDD  -> core
  }

  sensorOff();

  // TWIM settings are retained while disabled; pins are only taken over when ENABLE is set
  NRF_TWIM0->PSEL.SCL   = SENS_SCL_PIN;
  NRF_TWIM0->PSEL.SDA   = SENS_SDA_PIN;
  NRF_TWIM0->FREQUENCY  = TWIM_FREQUENCY_FREQUENCY_K400;
  NRF_TWIM0->ADDRESS    = SHT41_ADDR;
  NRF_TWIM0->TXD.PTR    = (uint32_t)g_i2cBuf;
  NRF_TWIM0->TXD.MAXCNT = 1;
  NRF_TWIM0->RXD.PTR    = (uint32_t)g_i2cBuf;
  NRF_TWIM0->RXD.MAXCNT = 6;

  Bluefruit.autoConnLed(false);                  // no blinking LED while advertising
  Bluefruit.begin();
  sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE); // keep REG1 DC/DC on under SoftDevice
  Bluefruit.setTxPower(TX_POWER_DBM);
  Bluefruit.setName(DEVICE_NAME);

  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);
  Bluefruit.Advertising.restartOnDisconnect(false);

  measureAndPublish(true);                       // start advertising even if first read fails
}

void loop() {
  delay(MEAS_INTERVAL_MS);                       // tickless idle -> System ON sleep
  measureAndPublish();
}
