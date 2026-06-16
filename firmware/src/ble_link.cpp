// ble_link.cpp — BTHome advertising OR connectable GATT server (compile switch).
//
// Built against NimBLE-Arduino 2.x (the version that supports the ESP32-C6
// radio). NimBLE is far smaller than the bluedroid stack — the same library
// HiveScale uses for its in-hive BLE bridge.
#include "ble_link.h"
#include "config.h"

#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <math.h>

namespace ble {

// ── BTHome v2 constants ──
static const NimBLEUUID BTHOME_UUID((uint16_t)0xFCD2);
static constexpr uint8_t BTHOME_DEVINFO = 0x40; // v2, unencrypted, not trigger
// BTHome object ids (subset we emit).
static constexpr uint8_t OBJ_PACKET_ID = 0x00; // uint8
static constexpr uint8_t OBJ_BATTERY   = 0x01; // uint8  %
static constexpr uint8_t OBJ_TEMP      = 0x02; // sint16 0.01 °C
static constexpr uint8_t OBJ_HUMIDITY  = 0x03; // uint16 0.01 %
static constexpr uint8_t OBJ_VOLTAGE   = 0x0C; // uint16 0.001 V (blob/GATT only;
                                               // omitted from adv — see buildBtHome)

// 16-bit "no registered company" id, matching HiveScale's bridge default. The
// scan-response manufacturer blob below uses it so a HiveScale-side parser
// (mirroring firmware/src/ble_sensor.cpp) can pick the packet out.
static constexpr uint16_t COMPANY_ID = 0xFFFF;
static constexpr uint8_t BLOB_VERSION = 0x01;

// ── Custom GATT service (full measurement as JSON) ──
static const char* SVC_HIVEINSIDE   = "8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01";
static const char* CHR_MEASUREMENT  = "8e8b0002-7a1c-4b9e-9a2f-1d6e0b9c1a01";

static int16_t  s16(float v, float scale) { return (int16_t)lroundf(v * scale); }
static uint16_t u16(float v, float scale) {
  long x = lroundf(v * scale);
  if (x < 0) x = 0;
  if (x > 65535) x = 65535;
  return (uint16_t)x;
}
static int8_t i8clamp(float v) {
  if (isnan(v) || v < -128.0f) return -128;
  if (v > 127.0f) return 127;
  return (int8_t)lroundf(v);
}

// Pack the full summary into the manufacturer-data blob (little-endian). Layout
// is documented in docs/esp32c6-prototype.md so a receiver can decode it.
static size_t packBlob(const Measurement& m, uint8_t* out) {
  size_t i = 0;
  auto putU16 = [&](uint16_t v) { out[i++] = v & 0xFF; out[i++] = v >> 8; };
  auto putI16 = [&](int16_t v) { putU16((uint16_t)v); };

  putU16(COMPANY_ID);
  out[i++] = BLOB_VERSION;
  uint8_t flags = (m.sht_ok ? 0x01 : 0) | (m.accel_ok ? 0x02 : 0) |
                  (m.mic_ok ? 0x04 : 0) | (m.battery_ok ? 0x08 : 0);
  out[i++] = flags;
  putI16(m.sht_ok ? s16(m.temp_c, 100.0f) : 0);
  putU16(m.sht_ok ? u16(m.humidity_pct, 100.0f) : 0);
  putU16(m.battery_ok ? u16(m.battery_v, 1000.0f) : 0); // mV
  out[i++] = m.battery_pct;
  putU16(m.accel_ok ? u16(m.accel_rms_mg, 10.0f) : 0);   // 0.1 mg units
  putU16(m.accel_ok ? u16(m.accel_peak_mg, 10.0f) : 0);
  putU16(m.accel_ok ? u16(m.accel_bands.swarm_mg, 10.0f) : 0);
  putU16(m.accel_ok ? u16(m.accel_bands.fanning_mg, 10.0f) : 0);
  putU16(m.accel_ok ? u16(m.accel_bands.activity_mg, 10.0f) : 0);
  out[i++] = (uint8_t)i8clamp(m.mic_rms_dbfs);  // dBFS, int8
  out[i++] = (uint8_t)i8clamp(m.mic_peak_dbfs);
  out[i++] = (uint8_t)i8clamp(m.mic_bands.sub_bass_dbfs);
  out[i++] = (uint8_t)i8clamp(m.mic_bands.hum_dbfs);
  out[i++] = (uint8_t)i8clamp(m.mic_bands.piping_dbfs);
  out[i++] = (uint8_t)i8clamp(m.mic_bands.stress_dbfs);
  out[i++] = (uint8_t)i8clamp(m.mic_bands.high_dbfs);
  return i; // ~28 bytes, fits a 31-byte scan-response payload
}

// Build the BTHome v2 service-data payload (device-info byte + objects, sorted
// ascending by object id as the spec requires). The 0xFCD2 UUID is prepended by
// NimBLE's setServiceData().
//
// Budget note: the primary advertising packet (built in publish()) is
// flags(3) + this service data + the local name, and must stay within the
// 31-byte legacy advertising limit. With the default name "HiveInside" (12 B
// on air) and flags (3 B) only 16 B are left, i.e. a service-data payload of at
// most 12. We therefore keep this to packet-id + battery% + temp + humidity
// (11 B). Raw battery *voltage* is intentionally omitted here — it is already
// carried in the scan-response manufacturer blob and the GATT JSON — so adding
// it back would overflow the packet (the original cause of NimBLE's
// "Data length exceeded").
static size_t buildBtHome(const Measurement& m, uint8_t* out) {
  size_t i = 0;
  out[i++] = BTHOME_DEVINFO;
  out[i++] = OBJ_PACKET_ID; out[i++] = m.packet_id;
  if (m.battery_ok) { out[i++] = OBJ_BATTERY; out[i++] = m.battery_pct; }
  if (m.sht_ok) {
    int16_t t = s16(m.temp_c, 100.0f);
    out[i++] = OBJ_TEMP; out[i++] = t & 0xFF; out[i++] = (t >> 8) & 0xFF;
    uint16_t h = u16(m.humidity_pct, 100.0f);
    out[i++] = OBJ_HUMIDITY; out[i++] = h & 0xFF; out[i++] = (h >> 8) & 0xFF;
  }
  return i; // <= 11 B, leaving room for flags + name in the 31-byte packet
}

// Serialise the entire Measurement to JSON for the GATT characteristic.
static String measurementJson(const Measurement& m) {
  JsonDocument doc;
  doc["fw"] = HIVEINSIDE_FW_VERSION;
  doc["packet_id"] = m.packet_id;
  doc["sht_ok"] = m.sht_ok;
  doc["temp_c"] = m.temp_c;
  doc["humidity_percent"] = m.humidity_pct;
  doc["accel_ok"] = m.accel_ok;
  doc["accel_sample_rate_hz"] = m.accel_sample_rate_hz;
  doc["accel_sample_count"] = m.accel_sample_count;
  doc["accel_rms_mg"] = m.accel_rms_mg;
  doc["accel_peak_mg"] = m.accel_peak_mg;
  doc["accel_band_swarm_mg"] = m.accel_bands.swarm_mg;
  doc["accel_band_fanning_mg"] = m.accel_bands.fanning_mg;
  doc["accel_band_activity_mg"] = m.accel_bands.activity_mg;
  doc["mic_ok"] = m.mic_ok;
  doc["mic_sample_rate_hz"] = m.mic_sample_rate_hz;
  doc["mic_rms_dbfs"] = m.mic_rms_dbfs;
  doc["mic_peak_dbfs"] = m.mic_peak_dbfs;
  doc["mic_rms_normalized"] = m.mic_rms_normalized;
  doc["mic_band_sub_bass_dbfs"] = m.mic_bands.sub_bass_dbfs;
  doc["mic_band_hum_dbfs"] = m.mic_bands.hum_dbfs;
  doc["mic_band_piping_dbfs"] = m.mic_bands.piping_dbfs;
  doc["mic_band_stress_dbfs"] = m.mic_bands.stress_dbfs;
  doc["mic_band_high_dbfs"] = m.mic_bands.high_dbfs;
  doc["battery_ok"] = m.battery_ok;
  doc["battery_v"] = m.battery_v;
  doc["battery_percent"] = m.battery_pct;
  String out;
  serializeJson(doc, out);
  return out;
}

// ===========================================================================
#if BLE_MODE == BLE_MODE_GATT
// ---------------------------------------------------------------------------
// GATT server mode
// ---------------------------------------------------------------------------
static NimBLECharacteristic* chrBattery = nullptr;
static NimBLECharacteristic* chrTemp = nullptr;
static NimBLECharacteristic* chrHumidity = nullptr;
static NimBLECharacteristic* chrMeasurement = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    Serial.println("[BLE] central connected");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[BLE] central disconnected (reason %d); re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

void begin() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks(), true);
  server->advertiseOnDisconnect(true);

  // Standard Battery Service (0x180F / 0x2A19).
  NimBLEService* battSvc = server->createService(NimBLEUUID((uint16_t)0x180F));
  chrBattery = battSvc->createCharacteristic(NimBLEUUID((uint16_t)0x2A19),
                  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  battSvc->start();

  // Environmental Sensing (0x181A): temperature (0x2A6E) + humidity (0x2A6F).
  NimBLEService* envSvc = server->createService(NimBLEUUID((uint16_t)0x181A));
  chrTemp = envSvc->createCharacteristic(NimBLEUUID((uint16_t)0x2A6E),
               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chrHumidity = envSvc->createCharacteristic(NimBLEUUID((uint16_t)0x2A6F),
                   NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  envSvc->start();

  // Custom HiveInside service: full measurement JSON (read + notify).
  NimBLEService* hiSvc = server->createService(SVC_HIVEINSIDE);
  chrMeasurement = hiSvc->createCharacteristic(CHR_MEASUREMENT,
                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  hiSvc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_DEVICE_NAME);
  adv->addServiceUUID(hiSvc->getUUID());
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("[BLE] GATT server advertising (connectable)");
}

void publish(const Measurement& m) {
  if (m.battery_ok && chrBattery) {
    uint8_t pct = m.battery_pct;
    chrBattery->setValue(&pct, 1);
    chrBattery->notify();
  }
  if (m.sht_ok) {
    int16_t t = s16(m.temp_c, 100.0f); // ES temperature: sint16, 0.01 °C
    chrTemp->setValue((uint8_t*)&t, 2);
    chrTemp->notify();
    uint16_t h = u16(m.humidity_pct, 100.0f); // ES humidity: uint16, 0.01 %
    chrHumidity->setValue((uint8_t*)&h, 2);
    chrHumidity->notify();
  }
  String json = measurementJson(m);
  chrMeasurement->setValue((uint8_t*)json.c_str(), json.length());
  chrMeasurement->notify();
  Serial.printf("[BLE] GATT updated (%u-byte JSON)\n", (unsigned)json.length());
}

#else
// ---------------------------------------------------------------------------
// Advertising (BTHome v2) mode
// ---------------------------------------------------------------------------
void begin() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  Serial.println("[BLE] advertising mode (BTHome v2 broadcast)");
}

void publish(const Measurement& m) {
  uint8_t bth[24];
  size_t bthLen = buildBtHome(m, bth);
  uint8_t blob[31];
  size_t blobLen = packBlob(m, blob);

  // Main advertisement: flags + BTHome service data + (short) name.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setServiceData(BTHOME_UUID, bth, bthLen);
  advData.setName(BLE_DEVICE_NAME);

  // Scan response carries the full vibration + acoustic summary blob so an
  // active scanner (or a HiveScale bridge) still gets every band over the air.
  NimBLEAdvertisementData scanData;
  scanData.setManufacturerData(blob, blobLen);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->start();
  Serial.printf("[BLE] advertising BTHome (%u B) + blob (%u B) for %d ms\n",
                (unsigned)bthLen, (unsigned)blobLen, (int)ADV_BURST_MS);
  delay(ADV_BURST_MS);
  adv->stop();
}
#endif

const char* modeName() {
#if BLE_MODE == BLE_MODE_GATT
  return "GATT";
#else
  return "ADVERTISING";
#endif
}

} // namespace ble
