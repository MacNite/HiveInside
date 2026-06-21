// ble_link.cpp — connectable GATT server: standard Battery (0x180F) and
// Environmental Sensing (0x181A) services for generic clients, plus a custom
// HiveInside service whose JSON characteristic carries the full Measurement.
//
// Built against NimBLE-Arduino 2.x (the version that supports the ESP32-C6
// radio). NimBLE is far smaller than the bluedroid stack — the same library
// HiveScale uses for its in-hive BLE bridge.
#include "ble_link.h"
#include "config.h"

#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <math.h>
#if HIVEINSIDE_OTA_ENABLED
#include <Update.h>   // Arduino-ESP32 OTA updater (writes the inactive app slot)
#endif

namespace ble {

// ── Custom GATT service (full measurement as JSON) ──
static const char* SVC_HIVEINSIDE   = "8e8b0001-7a1c-4b9e-9a2f-1d6e0b9c1a01";
static const char* CHR_MEASUREMENT  = "8e8b0002-7a1c-4b9e-9a2f-1d6e0b9c1a01";
// Writable: uint32 LE seconds HiveScale wants this device to sleep before the
// next connection. Must match HiveScale firmware/src/ble_sensor.cpp.
static const char* CHR_SYNC         = "8e8b0003-7a1c-4b9e-9a2f-1d6e0b9c1a01";
#if HIVEINSIDE_OTA_ENABLED
// Firmware-over-BLE characteristics (same custom service). Must match the UUIDs
// in HiveScale firmware/src/ble_sensor.cpp (HI_OTA_*).
static const char* CHR_OTA_CTRL     = "8e8b0010-7a1c-4b9e-9a2f-1d6e0b9c1a01"; // write: framed control
static const char* CHR_OTA_DATA     = "8e8b0011-7a1c-4b9e-9a2f-1d6e0b9c1a01"; // write: payload stream
static const char* CHR_OTA_STATUS   = "8e8b0013-7a1c-4b9e-9a2f-1d6e0b9c1a01"; // read/notify: state+recv+err
#endif

static int16_t  s16(float v, float scale) { return (int16_t)lroundf(v * scale); }
static uint16_t u16(float v, float scale) {
  long x = lroundf(v * scale);
  if (x < 0) x = 0;
  if (x > 65535) x = 65535;
  return (uint16_t)x;
}

// ── Pairing mode ──────────────────────────────────────────────────────────
// Just a timer that suppresses deep sleep so HiveScale's provisioning portal can
// discover the device. The device is already advertising as connectable; the
// pairing window only keeps it awake.
static unsigned long s_pairingUntilMs = 0;

void enterPairingMode() {
  s_pairingUntilMs = millis() + PAIRING_WINDOW_MS;
  Serial.printf("[BLE] Pairing window open for %lu min (%lus)\n",
                PAIRING_WINDOW_MS / 60000UL, PAIRING_WINDOW_MS / 1000UL);
}

bool isPairingActive() {
  return millis() < s_pairingUntilMs;
}

void shutdown() {
  NimBLEDevice::deinit(true);
  Serial.println("[BLE] shutdown");
}

// Round helpers — keep the JSON compact and free of float-repr noise
// (e.g. -75.80000305). NaN passes through and ArduinoJson serialises it as
// null, which the HiveScale reader maps back to "field absent".
static float r2(float v) { return isnan(v) ? v : roundf(v * 100.0f) / 100.0f; }
static float r1(float v) { return isnan(v) ? v : roundf(v * 10.0f) / 10.0f; }

// Serialise the measurement to JSON for the GATT characteristic.
//
// IMPORTANT — a single GATT characteristic value is capped at 512 bytes by the
// Bluetooth spec (ATT max attribute value length). The previous full dump ran
// to ~625 bytes and NimBLE rejected it ("val > max"), so connecting clients got
// a truncated/empty read. We therefore emit only the fields the HiveScale GATT
// reader (firmware/src/ble_sensor.cpp::gattReadHiveInside) consumes, plus fw +
// packet_id for diagnostics. This lands around ~420 bytes — well under the cap.
// Dropped vs. the old payload: *_sample_rate_hz, *_sample_count,
// mic_rms_normalized, mic_peak_dbfs, battery_v, sht_ok (metadata the bridge does
// not use). Generic clients still get temp/humidity/battery from the standard
// Environmental-Sensing and Battery services.
static String measurementJson(const Measurement& m) {
  JsonDocument doc;
  doc["fw"] = HIVEINSIDE_FW_VERSION;
  doc["packet_id"] = m.packet_id;
  doc["temp_c"] = r2(m.temp_c);
  doc["humidity_percent"] = r2(m.humidity_pct);
  doc["accel_ok"] = m.accel_ok;
  doc["accel_rms_mg"] = r1(m.accel_rms_mg);
  doc["accel_peak_mg"] = r1(m.accel_peak_mg);
  doc["accel_band_swarm_mg"] = r1(m.accel_bands.swarm_mg);
  doc["accel_band_fanning_mg"] = r1(m.accel_bands.fanning_mg);
  doc["accel_band_activity_mg"] = r1(m.accel_bands.activity_mg);
  doc["mic_ok"] = m.mic_ok;
  doc["mic_rms_dbfs"] = r1(m.mic_rms_dbfs);
  doc["mic_band_sub_bass_dbfs"] = r1(m.mic_bands.sub_bass_dbfs);
  doc["mic_band_hum_dbfs"] = r1(m.mic_bands.hum_dbfs);
  doc["mic_band_piping_dbfs"] = r1(m.mic_bands.piping_dbfs);
  doc["mic_band_stress_dbfs"] = r1(m.mic_bands.stress_dbfs);
  doc["mic_band_high_dbfs"] = r1(m.mic_bands.high_dbfs);
  doc["battery_percent"] = m.battery_pct;
  String out;
  serializeJson(doc, out);
  return out;
}

// ===========================================================================
// GATT server
// ===========================================================================
static NimBLECharacteristic* chrBattery = nullptr;
static NimBLECharacteristic* chrTemp = nullptr;
static NimBLECharacteristic* chrHumidity = nullptr;
static NimBLECharacteristic* chrMeasurement = nullptr;

#if HIVEINSIDE_SYNC_ENABLED
static NimBLECharacteristic* chrSync = nullptr;
static volatile bool     s_syncReceived = false;  // a valid wake-sync was written
static volatile uint64_t s_syncSleepMs  = 0;       // clamped sleep request
static volatile uint8_t  s_centralCount = 0;       // connected centrals

// On write, decode the uint32 LE seconds and clamp into the allowed range.
class SyncCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 4) {
      Serial.printf("[BLE] sync write ignored (%u bytes)\n", (unsigned)v.size());
      return;
    }
    const uint8_t* p = v.data();
    uint32_t sec = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint64_t ms = (uint64_t)sec * 1000ULL;
    if (ms < SYNC_MIN_SLEEP_MS) ms = SYNC_MIN_SLEEP_MS;
    if (ms > SYNC_MAX_SLEEP_MS) ms = SYNC_MAX_SLEEP_MS;
    s_syncSleepMs  = ms;
    s_syncReceived = true;
    Serial.printf("[BLE] wake-sync received: sleep %lus\n", (unsigned long)(ms / 1000ULL));
  }
};
#endif

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    Serial.println("[BLE] central connected");
#if HIVEINSIDE_SYNC_ENABLED
    s_centralCount++;
#endif
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[BLE] central disconnected (reason %d); re-advertising\n", reason);
#if HIVEINSIDE_SYNC_ENABLED
    if (s_centralCount) s_centralCount--;
#endif
    NimBLEDevice::startAdvertising();
  }
};

// ===========================================================================
// Firmware-over-BLE (OTA target)
// ===========================================================================
// HiveScale streams a new image into CHR_OTA_DATA in order; we write each chunk
// straight to the inactive OTA slot via the Arduino Update API and keep a
// running CRC-32. CHR_OTA_CTRL frames BEGIN / END / ABORT; CHR_OTA_STATUS lets
// the central poll progress and the final result. Nothing is committed until
// END verifies the end-to-end CRC, so a dropped/corrupted transfer always leaves
// the device on its current image.
#if HIVEINSIDE_OTA_ENABLED

// Control opcodes (first byte of a CTRL write) — must match HiveScale.
static constexpr uint8_t OTA_OP_BEGIN = 0x01;  // + size(4 LE) + crc32(4 LE)
static constexpr uint8_t OTA_OP_END   = 0x03;  // finalize, verify, reboot
static constexpr uint8_t OTA_OP_ABORT = 0x04;  // cancel

// Status state byte. >= 0x10 is a terminal error (matches BeeCounter semantics).
static constexpr uint8_t OTA_IDLE      = 0x00;
static constexpr uint8_t OTA_RECEIVING = 0x01;
static constexpr uint8_t OTA_DONE      = 0x02;
static constexpr uint8_t OTA_ERR_BEGIN = 0x10;
static constexpr uint8_t OTA_ERR_SEQ   = 0x11;
static constexpr uint8_t OTA_ERR_WRITE = 0x12;
static constexpr uint8_t OTA_ERR_CRC   = 0x13;
static constexpr uint8_t OTA_ERR_SIZE  = 0x14;
static constexpr uint8_t OTA_ERR_END   = 0x15;

static NimBLECharacteristic* chrOtaStatus = nullptr;
static volatile uint8_t  s_otaState = OTA_IDLE;
static volatile uint8_t  s_otaErr   = 0;
static uint32_t s_otaSize        = 0;          // expected image size
static volatile uint32_t s_otaRecv = 0;        // bytes written so far
static uint32_t s_otaExpectedCrc = 0;          // end-to-end CRC from BEGIN
static uint32_t s_otaRunningCrc  = 0xFFFFFFFFUL;
static unsigned long s_otaRebootAt = 0;        // 0 = no reboot pending

// CRC-32 (IEEE 802.3, reflected poly 0xEDB88320) — same as zlib.crc32 on the
// backend and beecnt::crc32_buf on HiveScale, so a value verified here matches
// end to end. Used with init/final 0xFFFFFFFF (see the END handler).
static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return crc;
}

static void otaPublishStatus() {
  if (!chrOtaStatus) return;
  uint8_t b[6];
  b[0] = s_otaState;
  uint32_t r = s_otaRecv;
  b[1] = r & 0xFF; b[2] = (r >> 8) & 0xFF; b[3] = (r >> 16) & 0xFF; b[4] = (r >> 24) & 0xFF;
  b[5] = s_otaErr;
  chrOtaStatus->setValue(b, sizeof(b));
  chrOtaStatus->notify();
}

static void otaFail(uint8_t state) {
  if (s_otaState == OTA_RECEIVING) Update.abort();
  s_otaState = state;
  s_otaErr   = state;
  otaPublishStatus();
}

class OtaCtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    if (v.size() < 1) return;
    const uint8_t* p = v.data();
    switch (p[0]) {
      case OTA_OP_BEGIN: {
        if (v.size() < 9) { otaFail(OTA_ERR_BEGIN); return; }
        uint32_t size = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                        ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        uint32_t crc  = (uint32_t)p[5] | ((uint32_t)p[6] << 8) |
                        ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
        if (!Update.begin(size)) {
          Serial.printf("[OTA] Update.begin(%u) failed: %s\n",
                        (unsigned)size, Update.errorString());
          otaFail(OTA_ERR_BEGIN);
          return;
        }
        s_otaSize        = size;
        s_otaExpectedCrc = crc;
        s_otaRecv        = 0;
        s_otaRunningCrc  = 0xFFFFFFFFUL;
        s_otaErr         = 0;
        s_otaState       = OTA_RECEIVING;
        Serial.printf("[OTA] BEGIN size=%u crc=0x%08X\n", (unsigned)size, (unsigned)crc);
        otaPublishStatus();
        break;
      }
      case OTA_OP_END: {
        if (s_otaState != OTA_RECEIVING) return;
        if (s_otaRecv != s_otaSize) {
          Serial.printf("[OTA] size mismatch recv=%u expected=%u\n",
                        (unsigned)s_otaRecv, (unsigned)s_otaSize);
          otaFail(OTA_ERR_SIZE);
          return;
        }
        uint32_t finalCrc = s_otaRunningCrc ^ 0xFFFFFFFFUL;
        if (s_otaExpectedCrc != 0 && finalCrc != s_otaExpectedCrc) {
          Serial.printf("[OTA] CRC mismatch got=0x%08X expected=0x%08X\n",
                        (unsigned)finalCrc, (unsigned)s_otaExpectedCrc);
          otaFail(OTA_ERR_CRC);
          return;
        }
        if (!Update.end(true)) {
          Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
          s_otaState = OTA_ERR_END;
          s_otaErr   = OTA_ERR_END;
          otaPublishStatus();
          return;
        }
        Serial.println("[OTA] END ok — image verified, rebooting shortly");
        s_otaState   = OTA_DONE;
        otaPublishStatus();
        // Defer the reset so the central can read DONE; loopOta() handles it.
        s_otaRebootAt = millis() + 1500;
        break;
      }
      case OTA_OP_ABORT: {
        if (s_otaState == OTA_RECEIVING) Update.abort();
        s_otaState = OTA_IDLE;
        s_otaErr   = 0;
        s_otaRecv  = 0;
        otaPublishStatus();
        Serial.println("[OTA] ABORT");
        break;
      }
      default:
        break;
    }
  }
};

class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (s_otaState != OTA_RECEIVING) { otaFail(OTA_ERR_SEQ); return; }
    NimBLEAttValue v = c->getValue();
    size_t n = v.size();
    if (n == 0) return;
    const uint8_t* p = v.data();
    // Update.write takes a non-const pointer but does not modify the buffer.
    if (Update.write(const_cast<uint8_t*>(p), n) != n) {
      Serial.printf("[OTA] write failed at %u: %s\n",
                    (unsigned)s_otaRecv, Update.errorString());
      otaFail(OTA_ERR_WRITE);
      return;
    }
    s_otaRunningCrc = crc32_update(s_otaRunningCrc, p, n);
    s_otaRecv += n;
  }
};

bool isOtaActive() {
  return s_otaState == OTA_RECEIVING || (s_otaState == OTA_DONE && s_otaRebootAt != 0);
}

void loopOta() {
  if (s_otaState == OTA_DONE && s_otaRebootAt != 0 &&
      (long)(millis() - s_otaRebootAt) >= 0) {
    Serial.println("[OTA] rebooting into new image");
    Serial.flush();
    NimBLEDevice::deinit(true);
    delay(50);
    ESP.restart();
  }
}
#endif  // HIVEINSIDE_OTA_ENABLED

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
#if HIVEINSIDE_SYNC_ENABLED
  // Wake-sync: HiveScale writes the next sleep duration here each cycle.
  chrSync = hiSvc->createCharacteristic(CHR_SYNC,
               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ);
  chrSync->setCallbacks(new SyncCallbacks());
#endif
#if HIVEINSIDE_OTA_ENABLED
  // Firmware-over-BLE: control + payload (write) and a readable/notifiable status.
  NimBLECharacteristic* chrOtaCtrl = hiSvc->createCharacteristic(CHR_OTA_CTRL,
               NIMBLE_PROPERTY::WRITE);
  chrOtaCtrl->setCallbacks(new OtaCtrlCallbacks());
  NimBLECharacteristic* chrOtaData = hiSvc->createCharacteristic(CHR_OTA_DATA,
               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrOtaData->setCallbacks(new OtaDataCallbacks());
  chrOtaStatus = hiSvc->createCharacteristic(CHR_OTA_STATUS,
               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  otaPublishStatus();
#endif
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
  // A GATT characteristic value cannot exceed 512 bytes (ATT spec limit). Skip
  // rather than let NimBLE reject it and leave clients with a stale value.
  if (json.length() > 512) {
    Serial.printf("[BLE] GATT JSON too large (%u B > 512); not updated — trim measurementJson()\n",
                  (unsigned)json.length());
    return;
  }
  chrMeasurement->setValue((uint8_t*)json.c_str(), json.length());
  chrMeasurement->notify();
  Serial.printf("[BLE] GATT updated (%u-byte JSON)\n", (unsigned)json.length());
}

#if HIVEINSIDE_SYNC_ENABLED
bool syncWakeMs(uint64_t* outMs) {
  if (!s_syncReceived) return false;
  if (outMs) *outMs = s_syncSleepMs;
  return true;
}

bool isCentralConnected() {
  return s_centralCount > 0;
}
#endif

const char* modeName() {
  return "GATT";
}

} // namespace ble
