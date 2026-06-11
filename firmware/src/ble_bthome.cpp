// ble_bthome.cpp — BTHome v2 advertiser on the Adafruit Bluefruit nRF52 stack.
//
// Payload format (BTHome v2, unencrypted):
//   [device-info byte = 0x40]  then repeated  [object-id][value...]
// Object IDs and scaling used here:
//   0x00 packet-id     uint8
//   0x01 battery %     uint8
//   0x02 temperature   int16   x 0.01 °C
//   0x03 humidity      uint16  x 0.01 %
//   0x04 pressure      uint24  x 0.01 hPa
//   0x51 acceleration  uint16  x 0.001 (we reuse for accel RMS, mg)
// Acoustic bands are sent as generic "raw"/custom values; HA shows them as
// sensors once a small template/BTHome config maps them. See docs/homeassistant.md
#include "ble_bthome.h"
#include "config.h"

#include <bluefruit.h>

static uint8_t packetId = 0;

void bleInit() {
  Bluefruit.begin();
  Bluefruit.setTxPower(0);          // 0 dBm — lowest TX current (~4.8 mA)
  Bluefruit.setName(BTHOME_DEVICE_NAME);
  Bluefruit.autoConnLed(false);     // save power: no advertising LED
}

// Append a 16-bit little-endian value.
static void put16(uint8_t* buf, uint8_t& i, int16_t v) {
  buf[i++] = (uint8_t)(v & 0xFF);
  buf[i++] = (uint8_t)((v >> 8) & 0xFF);
}

void bleAdvertise(const Measurement& m) {
  // ---- Build BTHome v2 service data ----
  // UUID 0xFCD2, then device-info byte, then objects.
  uint8_t payload[31];
  uint8_t i = 0;

  payload[i++] = 0x40;  // BTHome device info: v2, unencrypted, regular interval

  // packet id (helps HA dedupe)
  payload[i++] = 0x00;
  payload[i++] = packetId++;

  // battery %
  payload[i++] = 0x01;
  payload[i++] = m.battery_pct;

  // temperature x0.01 °C
  if (m.sht_ok) {
    payload[i++] = 0x02;
    put16(payload, i, (int16_t)lround(m.temperature_c * 100.0));
  }

  // humidity x0.01 %
  if (m.sht_ok) {
    payload[i++] = 0x03;
    uint16_t h = (uint16_t)lround(m.humidity_pct * 100.0);
    payload[i++] = h & 0xFF;
    payload[i++] = (h >> 8) & 0xFF;
  }

  // pressure x0.01 hPa (uint24)
  if (m.lps_ok) {
    payload[i++] = 0x04;
    uint32_t p = (uint32_t)lround(m.pressure_hpa * 100.0);
    payload[i++] = p & 0xFF;
    payload[i++] = (p >> 8) & 0xFF;
    payload[i++] = (p >> 16) & 0xFF;
  }

  // acceleration RMS (mg) via BTHome 0x51 "acceleration" x0.001 -> send raw mg
  if (m.accel_ok) {
    payload[i++] = 0x51;
    uint16_t a = (uint16_t)lround(m.accel_rms_mg);  // mg, capped to 65535
    payload[i++] = a & 0xFF;
    payload[i++] = (a >> 8) & 0xFF;
  }

  // ---- Push into the advertising packet ----
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, payload, i);

  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.setInterval(160, 160);  // 100 ms (units of 0.625 ms)
  Bluefruit.Advertising.setFastTimeout(ADV_DURATION_MS / 1000);
  Bluefruit.Advertising.start(ADV_DURATION_MS / 1000);

  delay(ADV_DURATION_MS);
  Bluefruit.Advertising.stop();
}

void bleStop() {
  Bluefruit.Advertising.stop();
}
