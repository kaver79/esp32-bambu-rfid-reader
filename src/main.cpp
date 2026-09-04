#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>

#include "config.h"
#include "web_ui.h"

#if RFID_READER == READER_RC522
#include <MFRC522.h>
MFRC522 reader(RFID_SS_PIN, RFID_RST_PIN);
#elif RFID_READER == READER_PN532
#include <Adafruit_PN532.h>
Adafruit_PN532 reader(RFID_SS_PIN, &SPI);
#elif RFID_READER == READER_PN5180
#include <PN5180ISO14443.h>
PN5180ISO14443 reader(RFID_SS_PIN, PN5180_BUSY_PIN, PN5180_RST_PIN);
#else
#error "RFID_READER must be READER_RC522, READER_PN532, or READER_PN5180"
#endif

static constexpr size_t BLOCK_COUNT = 64;
static constexpr size_t BLOCK_SIZE = 16;
static constexpr size_t DUMP_SIZE = BLOCK_COUNT * BLOCK_SIZE;
static constexpr uint32_t JOB_TIMEOUT_MS = 15000;
static constexpr uint32_t AUTO_SCAN_INTERVAL_MS = 350;
static constexpr uint32_t TAG_REMOVAL_MS = 1200;
static const uint8_t FUID_FACTORY_UID[4] = {0xAA, 0x55, 0xC3, 0x96};

static uint8_t dumpData[BLOCK_COUNT][BLOCK_SIZE];
static bool blockValid[BLOCK_COUNT];
static uint8_t lastUid[4];
static bool tagAvailable = false;
static uint8_t libraryDump[DUMP_SIZE];
static bool libraryDumpLoaded = false;
static String libraryDumpName;

enum class JobType { Idle, Scan, Write, CuidPreflight, CuidTest };
static JobType jobType = JobType::Idle;
static uint32_t jobDeadline = 0;
static String jobMessage = "Idle";
static bool jobSucceeded = false;

static WebServer server(80);
static Preferences preferences;
static String accessPointName;
static bool firmwareUploadAuthorized = false;
static bool firmwareUploadFailed = false;
static String firmwareUploadError;
static bool autoScanEnabled = true;
static bool autoTagLatched = false;
static uint8_t autoTagUid[4];
static uint32_t autoNextScanAt = 0;
static uint32_t autoLastSeenAt = 0;
#if RFID_READER == READER_PN5180
static uint8_t pn5180PresenceMisses = 0;
#endif
static bool writableCandidateAvailable = false;
static bool writableCandidateReady = false;
static bool writableCandidateFactoryUid = false;
static uint8_t writableCandidateUid[4];
static uint8_t writableCandidateSectors = 0;
static uint8_t cuidTestExpectedUid[4];
static bool unsupportedUidAvailable = false;
static uint8_t unsupportedUid[10];
static uint8_t unsupportedUidLength = 0;
static uint32_t unsupportedUidLastSeenAt = 0;
static bool readerReady = true;

static const uint8_t BAMBU_SALT[16] = {
    0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff,
    0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96};

static bool hmacSha256(const uint8_t *key, size_t keyLen,
                       const uint8_t *input, size_t inputLen,
                       uint8_t output[32]) {
  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return sha256 && mbedtls_md_hmac(sha256, key, keyLen, input, inputLen, output) == 0;
}

// RFC 5869 HKDF-SHA256. The 96 output bytes are sixteen six-byte Key A values.
static bool deriveBambuKeyA(const uint8_t uid[4], uint8_t keys[16][6]) {
  static const uint8_t info[] = {'R', 'F', 'I', 'D', '-', 'A', 0x00};
  uint8_t prk[32], output[96], previous[32];
  size_t previousLen = 0, outputLen = 0;
  if (!hmacSha256(BAMBU_SALT, sizeof(BAMBU_SALT), uid, 4, prk)) return false;
  for (uint8_t counter = 1; outputLen < sizeof(output); ++counter) {
    uint8_t message[sizeof(previous) + sizeof(info) + 1];
    size_t messageLen = 0;
    if (previousLen) {
      memcpy(message, previous, previousLen);
      messageLen += previousLen;
    }
    memcpy(message + messageLen, info, sizeof(info));
    messageLen += sizeof(info);
    message[messageLen++] = counter;
    if (!hmacSha256(prk, sizeof(prk), message, messageLen, previous)) return false;
    previousLen = sizeof(previous);
    const size_t amount = min(sizeof(previous), sizeof(output) - outputLen);
    memcpy(output + outputLen, previous, amount);
    outputLen += amount;
  }
  memcpy(keys, output, sizeof(output));
  return true;
}

static String hexString(const uint8_t *bytes, size_t length) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += HEX_DIGITS[bytes[i] >> 4];
    result += HEX_DIGITS[bytes[i] & 0x0f];
  }
  return result;
}

static void recordUnsupportedUid(const uint8_t *uid, uint8_t length) {
  unsupportedUidLength = min(length, static_cast<uint8_t>(sizeof(unsupportedUid)));
  memcpy(unsupportedUid, uid, unsupportedUidLength);
  unsupportedUidAvailable = true;
  unsupportedUidLastSeenAt = millis();
  jobSucceeded = false;
  jobMessage = "Detected " + String(length) + "-byte UID " +
               hexString(uid, unsupportedUidLength) +
               "; Bambu MIFARE Classic tags require a four-byte UID";
}

static void printHex(const uint8_t *bytes, size_t length, char separator = ' ') {
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] < 0x10) Serial.print('0');
    Serial.print(bytes[i], HEX);
    if (separator && i + 1 < length) Serial.print(separator);
  }
}

static String jsonEscape(const String &input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '\\' || c == '"') { output += '\\'; output += c; }
    else if (c == '\n') output += "\\n";
    else if (c == '\r') output += "\\r";
    else if (static_cast<uint8_t>(c) >= 0x20) output += c;
  }
  return output;
}

static String asciiField(const uint8_t *data, size_t block, size_t offset,
                         size_t length, const bool *valid = nullptr) {
  if (valid && !valid[block]) return String("<unread>");
  String value;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = data[block * BLOCK_SIZE + offset + i];
    if (c == 0x00 || c == 0xff) break;
    value += (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
  }
  value.trim();
  return value;
}

static uint16_t little16(const uint8_t *data, size_t block, size_t offset) {
  const size_t pos = block * BLOCK_SIZE + offset;
  return static_cast<uint16_t>(data[pos]) |
         (static_cast<uint16_t>(data[pos + 1]) << 8);
}

static String decodedTagJson(const uint8_t *data, const uint8_t uid[4],
                             const bool *valid = nullptr) {
  float diameter = 0;
  memcpy(&diameter, data + 5 * BLOCK_SIZE + 8, sizeof(diameter));
  String json = "{\"uid\":\"" + hexString(uid, 4) + "\"";
  json += ",\"variant\":\"" + jsonEscape(asciiField(data, 1, 0, 8, valid)) + "\"";
  json += ",\"materialId\":\"" + jsonEscape(asciiField(data, 1, 8, 8, valid)) + "\"";
  json += ",\"type\":\"" + jsonEscape(asciiField(data, 2, 0, 16, valid)) + "\"";
  json += ",\"detailedType\":\"" + jsonEscape(asciiField(data, 4, 0, 16, valid)) + "\"";
  json += ",\"color\":\"#" + hexString(data + 5 * BLOCK_SIZE, 4) + "\"";
  json += ",\"weight\":" + String(little16(data, 5, 4));
  json += ",\"diameter\":\"" + String(diameter, 3) + "\"";
  json += ",\"production\":\"" + jsonEscape(asciiField(data, 12, 0, 16, valid)) + "\"}";
  return json;
}

static void printDecodedData(const uint8_t uid[4]) {
  const uint8_t *data = &dumpData[0][0];
  Serial.println(F("\nDecoded Bambu fields"));
  Serial.print(F("UID:              ")); printHex(uid, 4, 0); Serial.println();
  Serial.print(F("Variant ID:       ")); Serial.println(asciiField(data, 1, 0, 8, blockValid));
  Serial.print(F("Material ID:      ")); Serial.println(asciiField(data, 1, 8, 8, blockValid));
  Serial.print(F("Filament type:    ")); Serial.println(asciiField(data, 2, 0, 16, blockValid));
  Serial.print(F("Detailed type:    ")); Serial.println(asciiField(data, 4, 0, 16, blockValid));
  if (blockValid[5]) {
    float diameter = 0;
    memcpy(&diameter, data + 5 * BLOCK_SIZE + 8, sizeof(diameter));
    Serial.print(F("Color RGBA:       #")); printHex(data + 5 * BLOCK_SIZE, 4, 0); Serial.println();
    Serial.print(F("Nominal weight:   ")); Serial.print(little16(data, 5, 4)); Serial.println(F(" g"));
    Serial.print(F("Diameter:         ")); Serial.print(diameter, 3); Serial.println(F(" mm"));
  }
  Serial.print(F("Production:       ")); Serial.println(asciiField(data, 12, 0, 16, blockValid));
}

#if RFID_READER == READER_PN5180
static bool pn5180PrepareTag(const uint8_t expectedUid[4], uint8_t selectedUid[4]) {
  // A PN5180 hardware reset clears Crypto1 and its RF field. The library's
  // readCardSerial path is more reliable on this board than manually turning
  // RF off before reset. Keep one explicit field-cycle fallback for a PICC
  // that did not leave its authenticated state on the first reset.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (attempt != 0) {
      reader.writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
      reader.setRF_off();
      delay(30);
    }
    reader.reset();
    delay(5);
    uint8_t uid[10] = {};
    if (reader.readCardSerial(uid) == 4) {
      memcpy(selectedUid, uid, 4);
      return !expectedUid || memcmp(selectedUid, expectedUid, 4) == 0;
    }
    delay(10);
  }
  return false;
}

static bool pn5180TagStillPresent(const uint8_t expectedUid[4]) {
  // The inspected card is left in HALT. WUPA provides a lightweight presence
  // check without repeating MIFARE authentication or reading any blocks.
  reader.reset();
  uint8_t activation[10] = {};
  if (reader.activateTypeA(activation, 1) != 4) return false;
  const bool matches = memcmp(activation + 3, expectedUid, 4) == 0;
  reader.mifareHalt();
  return matches;
}

static bool pn5180Authenticate(const uint8_t uid[4], uint8_t block,
                               const uint8_t key[6]) {
  const int16_t status =
      reader.mifareAuthenticate(block, key, MIFARE_CLASSIC_KEYA, uid);
  if (status != 0) {
    Serial.print(F("PN5180 authentication failed at block "));
    Serial.print(block);
    Serial.print(F(" (status "));
    Serial.print(status);
    Serial.println(')');
  }
  return status == 0;
}

static bool pn5180WriteBlock(uint8_t block, const uint8_t data[16]) {
  return (reader.mifareBlockWrite16(block, data) & 0x0f) == 0x0a;
}

static bool pn5180ReadBlock(uint8_t block, uint8_t data[16]) {
  if (reader.mifareBlockRead(block, data)) return true;
  uint32_t rxStatus = 0;
  reader.readRegister(RX_STATUS, &rxStatus);
  Serial.print(F("PN5180 read failed at block "));
  Serial.print(block);
  Serial.print(F(" (RX_STATUS 0x"));
  Serial.print(rxStatus, HEX);
  Serial.println(')');
  return false;
}

static bool pn5180RestoreBlock0(const uint8_t temporaryUid[4],
                                const uint8_t originalUid[4],
                                const uint8_t originalBlock0[16],
                                const uint8_t key[6]) {
  // A missing write ACK or a failed immediate reselect is ambiguous on this
  // PN5180 module. Probe both possible UIDs on every bounded attempt. If the
  // original block is already back, return success; otherwise reissue the
  // restore only after authenticating the temporary UID.
  for (uint8_t restoreAttempt = 0; restoreAttempt < 3; ++restoreAttempt) {
    for (uint8_t verifyAttempt = 0; verifyAttempt < 3; ++verifyAttempt) {
      uint8_t restoredUid[4] = {};
      if (pn5180PrepareTag(originalUid, restoredUid) &&
          pn5180Authenticate(restoredUid, 0, key)) {
        uint8_t restoredBlock0[16] = {};
        if (pn5180ReadBlock(0, restoredBlock0) &&
            memcmp(restoredBlock0, originalBlock0, 16) == 0) return true;
      }
    }

    uint8_t selectedTemporaryUid[4] = {};
    bool temporaryReady = false;
    for (uint8_t selectAttempt = 0; selectAttempt < 3 && !temporaryReady;
         ++selectAttempt) {
      temporaryReady = pn5180PrepareTag(temporaryUid, selectedTemporaryUid) &&
                       pn5180Authenticate(selectedTemporaryUid, 0, key);
    }
    if (!temporaryReady) continue;
    // Even a missing ACK is ambiguous: verify rather than assuming the
    // manufacturer-block restoration did not occur.
    pn5180WriteBlock(0, originalBlock0);
    delay(120);
  }
  return false;
}

static bool pn5180ReadExact(const uint8_t expectedUid[4], uint8_t keys[16][6],
                            String &error);
#endif

#if RFID_READER == READER_RC522
static bool detectTag(uint8_t uid[4], uint16_t timeoutMs = 50) {
  (void)timeoutMs;
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return false;
  if (reader.uid.size != 4) {
    recordUnsupportedUid(reader.uid.uidByte, reader.uid.size);
    reader.PICC_HaltA();
    return false;
  }
  unsupportedUidAvailable = false;
  memcpy(uid, reader.uid.uidByte, 4);
  return true;
}

static size_t readAllBlocks(uint8_t keys[16][6], const uint8_t uid[4]) {
  (void)uid;
  memset(blockValid, 0, sizeof(blockValid));
  size_t count = 0;
  for (uint8_t sector = 0; sector < 16; ++sector) {
    MFRC522::MIFARE_Key key;
    memcpy(key.keyByte, keys[sector], 6);
    const uint8_t firstBlock = sector * 4;
    if (reader.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, firstBlock,
                                &key, &reader.uid) == MFRC522::STATUS_OK) {
      for (uint8_t block = firstBlock; block < firstBlock + 4; ++block) {
        uint8_t buffer[18]; uint8_t length = sizeof(buffer);
        if (reader.MIFARE_Read(block, buffer, &length) == MFRC522::STATUS_OK && length >= BLOCK_SIZE) {
          memcpy(dumpData[block], buffer, BLOCK_SIZE);
          blockValid[block] = true; ++count;
        }
      }
    }
    reader.PCD_StopCrypto1();
  }
  reader.PICC_HaltA();
  return count;
}
#elif RFID_READER == READER_PN532
static bool detectTag(uint8_t uid[4], uint16_t timeoutMs = 50) {
  uint8_t detectedUid[10] = {};
  uint8_t uidLength = 0;
  if (!reader.readPassiveTargetID(PN532_MIFARE_ISO14443A, detectedUid,
                                  &uidLength, timeoutMs)) return false;
  if (uidLength != 4) {
    recordUnsupportedUid(detectedUid, uidLength);
    return false;
  }
  unsupportedUidAvailable = false;
  memcpy(uid, detectedUid, 4);
  return true;
}

static size_t readAllBlocks(uint8_t keys[16][6], const uint8_t uid[4]) {
  memset(blockValid, 0, sizeof(blockValid));
  size_t count = 0;
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t firstBlock = sector * 4;
    if (!reader.mifareclassic_AuthenticateBlock(const_cast<uint8_t *>(uid), 4,
                                                firstBlock, 0, keys[sector])) continue;
    for (uint8_t block = firstBlock; block < firstBlock + 4; ++block) {
      if (reader.mifareclassic_ReadDataBlock(block, dumpData[block])) {
        blockValid[block] = true; ++count;
      }
    }
  }
  return count;
}
#else
static bool detectTag(uint8_t uid[4], uint16_t timeoutMs = 50) {
  (void)timeoutMs;
  if (!readerReady) return false;
  uint8_t detectedUid[4] = {};
  reader.reset();
  if (reader.readCardSerial(detectedUid) != 4) return false;
  unsupportedUidAvailable = false;
  memcpy(uid, detectedUid, 4);
  return true;
}

// PN5180 phase one is intentionally UID-only. MIFARE Classic block access
// requires the PN5180 native authentication command, which the selected
// Arduino library does not expose yet. Never attempt unauthenticated reads.
static size_t readAllBlocks(uint8_t keys[16][6], const uint8_t uid[4]) {
  memset(blockValid, 0, sizeof(blockValid));
  size_t count = 0;
  for (uint8_t sector = 0; sector < 16; ++sector) {
    uint8_t selectedUid[4] = {};
    if (!pn5180PrepareTag(uid, selectedUid) ||
        !pn5180Authenticate(selectedUid, sector * 4, keys[sector])) continue;
    for (uint8_t block = sector * 4; block < sector * 4 + 4; ++block) {
      if (pn5180ReadBlock(block, dumpData[block])) {
        blockValid[block] = true;
        ++count;
      }
    }
  }
  return count;
}
#endif

// This is intentionally read-only. The factory UID and default keys make a tag
// eligible for the guarded FUID flow; they do not prove its magic commands
// or compatibility with an AMS/AMS Lite.
static bool inspectBlankTarget(const uint8_t uid[4]) {
  static uint8_t defaultKey[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t authenticatedSectors = 0;
#if RFID_READER == READER_PN5180
  // Auto-scan normally performs only a bounded block-0 probe. Do not let that
  // lightweight refresh erase a completed preflight for the same UID; the
  // destructive test independently repeats the full preflight before writing.
  const bool retainCompletedPreflight =
      writableCandidateAvailable && writableCandidateSectors == 16 &&
      memcmp(writableCandidateUid, uid, sizeof(writableCandidateUid)) == 0;
#endif
#if RFID_READER == READER_RC522
  MFRC522::MIFARE_Key key;
  memcpy(key.keyByte, defaultKey, sizeof(defaultKey));
  for (uint8_t sector = 0; sector < 16; ++sector) {
    if (reader.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, sector * 4,
                                &key, &reader.uid) == MFRC522::STATUS_OK) {
      ++authenticatedSectors;
    }
    reader.PCD_StopCrypto1();
  }
  reader.PICC_HaltA();
#elif RFID_READER == READER_PN532
  for (uint8_t sector = 0; sector < 16; ++sector) {
    if (reader.mifareclassic_AuthenticateBlock(const_cast<uint8_t *>(uid), 4,
                                                sector * 4, 0, defaultKey)) {
      ++authenticatedSectors;
    }
  }
#else
  // Keep the PN5180 probe bounded. A failed MIFARE
  // authentication moves the card out of ACTIVE state; probing all sectors
  // here would monopolize HTTP/OTA while repeatedly attempting reselection.
  if (pn5180Authenticate(uid, 0, defaultKey)) authenticatedSectors = 1;
  // Leave the card halted so periodic WUPA can distinguish presence from
  // retained last-scan data without repeating this authentication probe.
  reader.mifareHalt();
  if (retainCompletedPreflight) authenticatedSectors = 16;
#endif
  memcpy(writableCandidateUid, uid, sizeof(writableCandidateUid));
  writableCandidateAvailable = true;
  writableCandidateSectors = authenticatedSectors;
  writableCandidateFactoryUid =
      memcmp(uid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0;
  writableCandidateReady = writableCandidateFactoryUid && authenticatedSectors == 16;
  tagAvailable = false;
#if RFID_READER == READER_PN5180
  if (authenticatedSectors == 16) {
    jobMessage = "PN5180 full preflight retained for the same UID; write test will repeat it";
  } else if (authenticatedSectors == 1) {
    jobMessage = "PN5180 block 0 accepts the factory key; full write eligibility is not established";
  } else {
    jobMessage = "PN5180 block 0 rejected the factory key; write eligibility is not established";
  }
#else
  if (writableCandidateReady) {
    jobMessage = "Blank FUID candidate: all 16 sectors accept the factory key";
  } else if (authenticatedSectors == 16) {
    jobMessage = "Likely CUID/rewritable tag " + hexString(uid, 4) +
                 ": not compatible with the Bambu AMS write flow";
  } else {
    jobMessage = "UID " + hexString(uid, 4) + ": " +
                 String(authenticatedSectors) +
                 "/16 sectors accept the factory key; write eligibility not established";
  }
#endif
  Serial.print(F("Blank target inspection: UID "));
  printHex(uid, 4, 0);
#if RFID_READER == READER_PN5180
  Serial.print(F(", block 0 factory key "));
  Serial.println(authenticatedSectors == 1 ? F("accepted") : F("rejected"));
#else
  Serial.print(F(", default-key sectors "));
  Serial.print(authenticatedSectors);
  Serial.println(F("/16 (read-only inspection)"));
#endif
  return writableCandidateReady;
}

static bool readCurrentTag(const uint8_t uid[4]) {
#if RFID_READER == READER_PN5180
  Serial.print(F("PN5180 ISO14443A UID: "));
  printHex(uid, 4, 0);
  Serial.println();
#endif
  if (memcmp(uid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0) {
    return inspectBlankTarget(uid);
  }
  writableCandidateAvailable = false;
  writableCandidateReady = false;
  writableCandidateFactoryUid = false;
  writableCandidateSectors = 0;
  uint8_t keys[16][6];
  if (!deriveBambuKeyA(uid, keys)) { jobMessage = "Key derivation failed"; return false; }
  size_t blocks = 0;
#if RFID_READER == READER_PN5180
  String pn5180ReadError;
  blocks = pn5180ReadExact(uid, keys, pn5180ReadError) ? BLOCK_COUNT : 0;
#else
  blocks = readAllBlocks(keys, uid);
#endif
  if (blocks != BLOCK_COUNT) {
#if RFID_READER == READER_PN532
    // A failed Bambu authentication can leave the target unusable for the next
    // key attempt. Reselect it before the read-only factory-key inspection.
    reader.SAMConfig();
    delay(20);
    uint8_t retryUid[4];
    if (detectTag(retryUid, 300) && memcmp(retryUid, uid, sizeof(retryUid)) == 0) {
      return inspectBlankTarget(uid);
    }
#elif RFID_READER == READER_PN5180
    // Reselect after failed Bambu-key attempts before probing a factory or
    // engineering tag with the default key.
    uint8_t retryUid[4] = {};
    if (pn5180PrepareTag(uid, retryUid)) return inspectBlankTarget(uid);
#endif
    jobMessage = "Read " + String(blocks) + "/64 blocks; keep the tag centered and retry";
    return false;
  }
  memcpy(lastUid, uid, 4);
  tagAvailable = true;
  printDecodedData(uid);
  jobMessage = "Read complete: " + hexString(uid, 4);
  return true;
}

static void latchAutoTag(const uint8_t uid[4]) {
  memcpy(autoTagUid, uid, 4);
  autoTagLatched = true;
  autoLastSeenAt = millis();
#if RFID_READER == READER_PN5180
  pn5180PresenceMisses = 0;
#endif
}

static void serviceAutoScan() {
  if (!readerReady || !autoScanEnabled || jobType != JobType::Idle) return;
#if RFID_READER == READER_PN5180
  if (autoTagLatched) {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - autoNextScanAt) < 0) return;
    autoNextScanAt = now + 700;
    if (pn5180TagStillPresent(autoTagUid)) {
      pn5180PresenceMisses = 0;
      autoLastSeenAt = millis();
      return;
    }
    if (++pn5180PresenceMisses < 3) return;
    pn5180PresenceMisses = 0;
    autoTagLatched = false;
    jobMessage = "Tag removed; showing last scanned data";
    jobSucceeded = false;
    return;
  }
#endif
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - autoNextScanAt) < 0) return;
  autoNextScanAt = now + AUTO_SCAN_INTERVAL_MS;

  uint8_t uid[4];
  if (detectTag(uid, 35)) {
    autoLastSeenAt = now;
    if (!autoTagLatched || memcmp(uid, autoTagUid, 4) != 0) {
      latchAutoTag(uid);
      jobMessage = "Automatically reading " + hexString(uid, 4) + "…";
      jobSucceeded = readCurrentTag(uid);
    }
  } else if (autoTagLatched && now - autoLastSeenAt >= TAG_REMOVAL_MS) {
    autoTagLatched = false;
    jobMessage = "Ready for the next tag";
    jobSucceeded = false;
  }
}

static bool validateLibraryDump(String &error) {
  const uint8_t bcc = libraryDump[0] ^ libraryDump[1] ^ libraryDump[2] ^ libraryDump[3];
  if (libraryDump[4] != bcc) { error = "Invalid manufacturer block (UID checksum mismatch)"; return false; }
  bool empty = true;
  for (size_t i = BLOCK_SIZE; i < DUMP_SIZE; ++i) {
    if (libraryDump[i] != 0x00 && libraryDump[i] != 0xff) { empty = false; break; }
  }
  if (empty) { error = "Dump contains no filament data"; return false; }
  uint8_t expectedKeys[16][6];
  if (!deriveBambuKeyA(libraryDump, expectedKeys)) {
    error = "Could not validate dump sector keys"; return false;
  }
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t *trailer = libraryDump + (sector * 4 + 3) * BLOCK_SIZE;
    if (memcmp(trailer, expectedKeys[sector], 6) != 0) {
      error = "Dump sector " + String(sector) + " does not contain the expected Key A";
      return false;
    }
  }
  return true;
}

static String urlEncodePath(const String &path) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(path.length() + 20);
  for (size_t i = 0; i < path.length(); ++i) {
    const uint8_t c = path[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') encoded += static_cast<char>(c);
    else { encoded += '%'; encoded += HEX_DIGITS[c >> 4]; encoded += HEX_DIGITS[c & 0x0f]; }
  }
  return encoded;
}

static bool downloadLibraryDump(const String &path, String &error) {
  if (WiFi.status() != WL_CONNECTED) { error = "ESP32 is not connected to the internet"; return false; }
  if (path.length() < 10 || path.length() > 400 || path.indexOf("..") >= 0 ||
      path.indexOf('\\') >= 0 || !path.endsWith("-dump.bin")) {
    error = "Rejected library path"; return false;
  }
  WiFiClientSecure client;
  client.setInsecure(); // GitHub rotates certificates; payload structure is validated below.
  client.setTimeout(12000);
  HTTPClient http;
  const String url = "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/" + urlEncodePath(path);
  if (!http.begin(client, url)) { error = "Could not initialize HTTPS"; return false; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { error = "GitHub download returned HTTP " + String(code); http.end(); return false; }
  const int length = http.getSize();
  if (length >= 0 && length != static_cast<int>(DUMP_SIZE)) {
    error = "Expected 1024 bytes, received " + String(length); http.end(); return false;
  }
  const size_t received = http.getStreamPtr()->readBytes(libraryDump, DUMP_SIZE);
  http.end();
  if (received != DUMP_SIZE) { error = "Download ended after " + String(received) + " bytes"; return false; }
  if (!validateLibraryDump(error)) return false;
  libraryDumpLoaded = true;
  const int slash = path.lastIndexOf('/');
  libraryDumpName = slash >= 0 ? path.substring(slash + 1) : path;
  return true;
}

#if RFID_READER == READER_PN532
static bool readableContentMatchesLibrary() {
  const uint8_t *readback = &dumpData[0][0];
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const size_t first = sector * 4 * BLOCK_SIZE;
    if (memcmp(readback + first, libraryDump + first, 3 * BLOCK_SIZE) != 0) return false;
    // Key A is deliberately unreadable on MIFARE Classic. Access bits and GPB are readable.
    if (memcmp(readback + first + 3 * BLOCK_SIZE + 6,
               libraryDump + first + 3 * BLOCK_SIZE + 6, 4) != 0) return false;
  }
  return true;
}

// Recovery is allowed only when every readable byte written before block 0
// already matches the selected signed dump. Manufacturer block 0 is excluded
// because an interrupted FUID personalization deliberately leaves the factory
// UID there until the final irreversible step.
static bool recoverableContentMatchesLibrary() {
  const uint8_t *readback = &dumpData[0][0];
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t firstBlock = sector * 4;
    for (uint8_t offset = 0; offset < 3; ++offset) {
      const uint8_t block = firstBlock + offset;
      if (block == 0) continue;
      if (memcmp(readback + block * BLOCK_SIZE,
                 libraryDump + block * BLOCK_SIZE, BLOCK_SIZE) != 0) return false;
    }
    const size_t trailer = (firstBlock + 3) * BLOCK_SIZE;
    // Key A is deliberately unreadable; access bits and GPB must still match.
    if (memcmp(readback + trailer + 6, libraryDump + trailer + 6, 4) != 0) return false;
  }
  return true;
}

static bool writeFinalUidAndVerify(const uint8_t targetUid[4],
                                   uint8_t sourceKeys[16][6], String &error) {
  if (!reader.mifareclassic_AuthenticateBlock(const_cast<uint8_t *>(targetUid),
                                               4, 0, 0, sourceKeys[0])) {
    error = "Could not authenticate sector 0 before the final UID write"; return false;
  }
  if (!reader.mifareclassic_WriteDataBlock(0, libraryDump)) {
    error = "Final UID write failed; keep this tag for recovery inspection"; return false;
  }

  delay(150);
  reader.SAMConfig();
  uint8_t newUid[4] = {};
  if (!detectTag(newUid, 800) || memcmp(newUid, libraryDump, 4) != 0) {
    error = "UID write returned, but the selected source UID could not be verified"; return false;
  }
  uint8_t keys[16][6];
  if (!deriveBambuKeyA(newUid, keys) || readAllBlocks(keys, newUid) != BLOCK_COUNT ||
      !readableContentMatchesLibrary()) {
    error = "UID changed, but full 1 KiB verification failed"; return false;
  }
  memcpy(lastUid, newUid, 4);
  tagAvailable = true;
  writableCandidateAvailable = false;
  writableCandidateReady = false;
  writableCandidateFactoryUid = false;
  writableCandidateSectors = 0;
  latchAutoTag(newUid);
  return true;
}

static bool writeExactFuid(String &error) {
  static uint8_t defaultKey[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t targetUid[4] = {};
  bool found = false;
  jobMessage = "Waiting for factory FUID AA55C396…";
  while (static_cast<int32_t>(jobDeadline - millis()) > 0) {
    server.handleClient();
    if (!detectTag(targetUid, 100)) continue;
    found = true;
    if (memcmp(targetUid, FUID_FACTORY_UID, 4) != 0) {
      error = "Refused UID " + hexString(targetUid, 4) + "; expected unused FUID AA55C396";
      return false;
    }
    break;
  }
  if (!found) { error = "Timed out waiting for an unused FUID"; return false; }

  uint8_t sourceKeys[16][6];
  if (!deriveBambuKeyA(libraryDump, sourceKeys)) {
    error = "Could not derive the selected dump keys"; return false;
  }

  // A prior attempt may have personalized every sector trailer and then failed
  // at block 0. Prove that exact state before retrying the UID-only final step.
  if (!reader.mifareclassic_AuthenticateBlock(targetUid, 4, 0, 0, defaultKey)) {
    jobMessage = "Factory key changed; validating interrupted-write recovery…";
    reader.SAMConfig();
    delay(20);
    uint8_t recoveryUid[4] = {};
    if (!detectTag(recoveryUid, 500) ||
        memcmp(recoveryUid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) != 0 ||
        readAllBlocks(sourceKeys, recoveryUid) != BLOCK_COUNT ||
        !recoverableContentMatchesLibrary()) {
      error = "Recovery refused: tag contents do not exactly match the selected dump";
      return false;
    }
    jobMessage = "Interrupted write verified; retrying final UID block…";
    reader.SAMConfig();
    delay(20);
    uint8_t finalUid[4] = {};
    if (!detectTag(finalUid, 500) ||
        memcmp(finalUid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) != 0) {
      error = "Recovery stopped: factory UID was not reselected before block 0";
      return false;
    }
    return writeFinalUidAndVerify(finalUid, sourceKeys, error);
  }

  // Block 0 is deliberately last: earlier failures do not consume the one-time UID.
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t first = sector * 4;
    if (!reader.mifareclassic_AuthenticateBlock(targetUid, 4, first, 0, defaultKey)) {
      error = "Factory-key authentication failed in sector " + String(sector); return false;
    }
    const uint8_t start = sector == 0 ? 1 : first;
    for (uint8_t block = start; block < first + 3; ++block) {
      if (!reader.mifareclassic_WriteDataBlock(block, libraryDump + block * BLOCK_SIZE)) {
        error = "Write failed at data block " + String(block); return false;
      }
    }
    const uint8_t trailer = first + 3;
    if (!reader.mifareclassic_WriteDataBlock(trailer, libraryDump + trailer * BLOCK_SIZE)) {
      error = "Write failed at sector trailer " + String(trailer); return false;
    }
    jobMessage = "Personalizing sector " + String(sector + 1) + "/16";
    delay(8);
  }

  return writeFinalUidAndVerify(targetUid, sourceKeys, error);
}

static bool writeCuidEngineeringTest(String &error) {
  static uint8_t defaultKeys[16][6];
  memset(defaultKeys, 0xff, sizeof(defaultKeys));
  uint8_t targetUid[4] = {};
  bool found = false;
  jobMessage = "Waiting for the selected CUID UID-test tag…";
  while (static_cast<int32_t>(jobDeadline - millis()) > 0) {
    server.handleClient();
    if (!detectTag(targetUid, 100)) continue;
    found = true;
    if (memcmp(targetUid, cuidTestExpectedUid, sizeof(targetUid)) != 0) {
      error = "CUID test refused: a different tag was detected"; return false;
    }
    if (memcmp(targetUid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0) {
      error = "CUID test refused: AA55C396 is reserved for the FUID flow"; return false;
    }
    break;
  }
  if (!found) { error = "Timed out waiting for the selected CUID test tag"; return false; }

  jobMessage = "CUID UID-test preflight: reading all blocks without writing…";
  if (readAllBlocks(defaultKeys, targetUid) != BLOCK_COUNT ||
      memcmp(dumpData[0], targetUid, 4) != 0 ||
      dumpData[0][4] != (targetUid[0] ^ targetUid[1] ^ targetUid[2] ^ targetUid[3])) {
    error = "CUID UID test refused: full factory-key read or manufacturer block check failed";
    return false;
  }
  uint8_t originalBlock0[BLOCK_SIZE];
  uint8_t temporaryBlock0[BLOCK_SIZE];
  memcpy(originalBlock0, dumpData[0], BLOCK_SIZE);
  memcpy(temporaryBlock0, originalBlock0, BLOCK_SIZE);
  temporaryBlock0[3] ^= 0x01;
  temporaryBlock0[4] = temporaryBlock0[0] ^ temporaryBlock0[1] ^
                      temporaryBlock0[2] ^ temporaryBlock0[3];
  uint8_t temporaryUid[4];
  memcpy(temporaryUid, temporaryBlock0, sizeof(temporaryUid));

  reader.SAMConfig();
  delay(20);
  uint8_t selectedUid[4] = {};
  if (!detectTag(selectedUid, 500) ||
      memcmp(selectedUid, cuidTestExpectedUid, sizeof(selectedUid)) != 0) {
    error = "CUID UID test stopped: the same tag was not reselected after preflight";
    return false;
  }

  if (!reader.mifareclassic_AuthenticateBlock(selectedUid, 4, 0, 0,
                                               defaultKeys[0])) {
    error = "CUID UID test could not authenticate block 0"; return false;
  }
  jobMessage = "Writing temporary CUID " + hexString(temporaryUid, 4) + "…";
  if (!reader.mifareclassic_WriteDataBlock(0, temporaryBlock0)) {
    error = "Temporary CUID write was rejected; original UID should be unchanged";
    return false;
  }

  delay(120);
  reader.SAMConfig();
  uint8_t detectedTemporaryUid[4] = {};
  if (!detectTag(detectedTemporaryUid, 1000) ||
      memcmp(detectedTemporaryUid, temporaryUid, sizeof(temporaryUid)) != 0) {
    error = "Temporary UID write returned but could not be verified; expected " +
            hexString(temporaryUid, 4);
    return false;
  }
  if (!reader.mifareclassic_AuthenticateBlock(detectedTemporaryUid, 4, 0, 0,
                                               defaultKeys[0])) {
    error = "Temporary UID verified, but block 0 could not be authenticated for restore";
    return false;
  }
  jobMessage = "Temporary UID verified; restoring " + hexString(targetUid, 4) + "…";
  if (!reader.mifareclassic_WriteDataBlock(0, originalBlock0)) {
    error = "Temporary UID verified, but restoring the original UID failed";
    return false;
  }

  delay(120);
  reader.SAMConfig();
  uint8_t restoredUid[4] = {};
  if (!detectTag(restoredUid, 1000) ||
      memcmp(restoredUid, targetUid, sizeof(restoredUid)) != 0 ||
      !reader.mifareclassic_AuthenticateBlock(restoredUid, 4, 0, 0,
                                              defaultKeys[0])) {
    error = "Original UID restore could not be verified"; return false;
  }
  uint8_t restoredBlock0[BLOCK_SIZE] = {};
  if (!reader.mifareclassic_ReadDataBlock(0, restoredBlock0) ||
      memcmp(restoredBlock0, originalBlock0, BLOCK_SIZE) != 0) {
    error = "Original UID returned, but manufacturer block verification failed";
    return false;
  }
  jobMessage = "CUID UID rewrite verified (" + hexString(targetUid, 4) + " → " +
               hexString(temporaryUid, 4) + " → restored)";
  return true;
}
#elif RFID_READER == READER_PN5180
static bool pn5180ReadExact(const uint8_t expectedUid[4], uint8_t keys[16][6],
                            String &error) {
  memset(blockValid, 0, sizeof(blockValid));
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t first = sector * 4;
    bool complete = false;
    for (uint8_t attempt = 0; attempt < 3 && !complete; ++attempt) {
      uint8_t selectedUid[4] = {};
      if (!pn5180PrepareTag(expectedUid, selectedUid) ||
          !pn5180Authenticate(selectedUid, first, keys[sector])) continue;
      complete = true;
      for (uint8_t block = first; block < first + 4; ++block) {
        if (!pn5180ReadBlock(block, dumpData[block])) {
          complete = false;
          break;
        }
        blockValid[block] = true;
      }
    }
    if (!complete) {
      error = "PN5180 could not read sector " + String(sector) + " after 3 attempts";
      return false;
    }
    server.handleClient();
  }
  return true;
}

static bool pn5180ContentMatchesLibrary(bool includeBlock0) {
  const uint8_t *readback = &dumpData[0][0];
  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t first = sector * 4;
    for (uint8_t offset = 0; offset < 3; ++offset) {
      const uint8_t block = first + offset;
      if (!includeBlock0 && block == 0) continue;
      if (memcmp(readback + block * BLOCK_SIZE,
                 libraryDump + block * BLOCK_SIZE, BLOCK_SIZE) != 0) return false;
    }
    const size_t trailer = (first + 3) * BLOCK_SIZE;
    if (memcmp(readback + trailer + 6, libraryDump + trailer + 6, 4) != 0) return false;
  }
  return true;
}

static bool pn5180WriteDataVerified(const uint8_t uid[4], uint8_t authBlock,
                                    const uint8_t key[6], uint8_t block,
                                    const uint8_t expected[16]) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    uint8_t selectedUid[4] = {};
    if (!pn5180PrepareTag(uid, selectedUid) ||
        !pn5180Authenticate(selectedUid, authBlock, key)) continue;
    pn5180WriteBlock(block, expected);  // ACK may be lost; readback is authoritative.
    delay(25);
    if (!pn5180PrepareTag(uid, selectedUid) ||
        !pn5180Authenticate(selectedUid, authBlock, key)) continue;
    uint8_t readback[16] = {};
    if (pn5180ReadBlock(block, readback) && memcmp(readback, expected, 16) == 0) {
      return true;
    }
  }
  return false;
}

static bool pn5180TrailerMatches(const uint8_t uid[4], uint8_t firstBlock,
                                 const uint8_t newKey[6], uint8_t trailer,
                                 const uint8_t expected[16]) {
  uint8_t selectedUid[4] = {};
  if (!pn5180PrepareTag(uid, selectedUid) ||
      !pn5180Authenticate(selectedUid, firstBlock, newKey)) return false;
  uint8_t readback[16] = {};
  return pn5180ReadBlock(trailer, readback) &&
         memcmp(readback + 6, expected + 6, 4) == 0;
}

static bool pn5180WriteTrailerVerified(const uint8_t uid[4], uint8_t firstBlock,
                                       const uint8_t oldKey[6],
                                       const uint8_t newKey[6], uint8_t trailer,
                                       const uint8_t expected[16]) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    // An earlier write may have succeeded despite a lost ACK.
    if (pn5180TrailerMatches(uid, firstBlock, newKey, trailer, expected)) return true;
    uint8_t selectedUid[4] = {};
    if (!pn5180PrepareTag(uid, selectedUid) ||
        !pn5180Authenticate(selectedUid, firstBlock, oldKey)) continue;
    pn5180WriteBlock(trailer, expected);
    delay(30);
  }
  return pn5180TrailerMatches(uid, firstBlock, newKey, trailer, expected);
}

static bool pn5180SectorMatchesLibrary(const uint8_t uid[4], uint8_t sector,
                                       const uint8_t key[6],
                                       bool &authenticated) {
  const uint8_t first = sector * 4;
  authenticated = false;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    uint8_t selectedUid[4] = {};
    if (!pn5180PrepareTag(uid, selectedUid) ||
        !pn5180Authenticate(selectedUid, first, key)) continue;
    authenticated = true;
    bool matches = true;
    const uint8_t start = sector == 0 ? 1 : first;
    for (uint8_t block = start; block < first + 3; ++block) {
      uint8_t readback[16] = {};
      if (!pn5180ReadBlock(block, readback) ||
          memcmp(readback, libraryDump + block * BLOCK_SIZE, 16) != 0) {
        matches = false;
        break;
      }
    }
    uint8_t trailerReadback[16] = {};
    if (matches && (!pn5180ReadBlock(first + 3, trailerReadback) ||
        memcmp(trailerReadback + 6,
               libraryDump + (first + 3) * BLOCK_SIZE + 6, 4) != 0)) {
      matches = false;
    }
    if (matches) return true;
  }
  return false;
}

static bool pn5180WriteFinalFuid(uint8_t sourceKeys[16][6], String &error) {
  uint8_t selectedUid[4] = {};
  if (!pn5180PrepareTag(FUID_FACTORY_UID, selectedUid) ||
      !pn5180Authenticate(selectedUid, 0, sourceKeys[0])) {
    error = "Could not authenticate sector 0 before the irreversible UID write";
    return false;
  }
  const bool writeAck = pn5180WriteBlock(0, libraryDump);
  delay(150);

  uint8_t newUid[4] = {};
  if (!pn5180PrepareTag(libraryDump, newUid)) {
    error = writeAck
        ? "Final UID write returned ACK, but the source UID could not be reselected"
        : "Final UID write result is unknown; keep the tag for recovery inspection";
    return false;
  }
  String verifyError;
  if (!pn5180ReadExact(newUid, sourceKeys, verifyError) ||
      !pn5180ContentMatchesLibrary(true)) {
    error = "UID changed, but full 1 KiB verification failed: " + verifyError;
    return false;
  }
  memcpy(lastUid, newUid, 4);
  tagAvailable = true;
  writableCandidateAvailable = false;
  writableCandidateReady = false;
  writableCandidateFactoryUid = false;
  writableCandidateSectors = 0;
  latchAutoTag(newUid);
  return true;
}

static bool writeExactFuid(String &error) {
  static const uint8_t defaultKey[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t sourceKeys[16][6];
  if (!deriveBambuKeyA(libraryDump, sourceKeys)) {
    error = "Could not derive the selected dump keys";
    return false;
  }

  uint8_t selectedUid[4] = {};
  if (!pn5180PrepareTag(FUID_FACTORY_UID, selectedUid)) {
    error = "PN5180 did not find exactly one factory FUID AA55C396";
    return false;
  }

  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t first = sector * 4;
    bool sourceAuthenticated = false;
    if (pn5180SectorMatchesLibrary(FUID_FACTORY_UID, sector,
                                   sourceKeys[sector], sourceAuthenticated)) {
      jobMessage = "PN5180 recovered verified source sector " +
                   String(sector + 1) + "/16";
      server.handleClient();
      continue;
    }
    if (sourceAuthenticated) {
      error = "Recovery refused: source-key sector " + String(sector) +
              " does not exactly match the selected dump";
      return false;
    }

    uint8_t factoryUid[4] = {};
    if (!pn5180PrepareTag(FUID_FACTORY_UID, factoryUid) ||
        !pn5180Authenticate(factoryUid, first, defaultKey)) {
      error = "Recovery refused: sector " + String(sector) +
              " accepts neither a verified source state nor the factory key";
      return false;
    }
    const uint8_t start = sector == 0 ? 1 : first;
    for (uint8_t block = start; block < first + 3; ++block) {
      if (!pn5180WriteDataVerified(FUID_FACTORY_UID, first, defaultKey, block,
                                   libraryDump + block * BLOCK_SIZE)) {
        error = "Write/readback failed at data block " + String(block) +
                " after 3 attempts";
        return false;
      }
    }
    const uint8_t trailer = first + 3;
    if (!pn5180WriteTrailerVerified(FUID_FACTORY_UID, first, defaultKey,
                                    sourceKeys[sector], trailer,
                                    libraryDump + trailer * BLOCK_SIZE)) {
      error = "Write/key/readback failed at sector trailer " + String(trailer) +
              " after 3 attempts";
      return false;
    }

    // Reuse the bounded recovery verifier rather than trusting one RF
    // exchange after a trailer/key transition.
    bool verifiedAuthentication = false;
    if (!pn5180SectorMatchesLibrary(FUID_FACTORY_UID, sector,
                                    sourceKeys[sector],
                                    verifiedAuthentication)) {
      error = "Source-key content verification failed in sector " + String(sector) +
              " after 3 attempts";
      return false;
    }
    jobMessage = "PN5180 personalized and verified sector " + String(sector + 1) + "/16";
    server.handleClient();
  }
  return pn5180WriteFinalFuid(sourceKeys, error);
}

static bool runCuidReadOnlyPreflight(String &error) {
  static uint8_t defaultKeys[16][6];
  memset(defaultKeys, 0xff, sizeof(defaultKeys));
  memset(blockValid, 0, sizeof(blockValid));
  uint8_t completedSectors = 0;

  for (uint8_t sector = 0; sector < 16; ++sector) {
    const uint8_t firstBlock = sector * 4;
    bool sectorComplete = false;
    bool selectedOnce = false;
    bool authenticatedOnce = false;
    uint8_t failedBlock = firstBlock;
    for (uint8_t attempt = 0; attempt < 3 && !sectorComplete; ++attempt) {
      for (uint8_t block = firstBlock; block < firstBlock + 4; ++block) {
        blockValid[block] = false;
      }
      uint8_t selectedUid[4] = {};
      if (!pn5180PrepareTag(cuidTestExpectedUid, selectedUid)) continue;
      selectedOnce = true;
      if (!pn5180Authenticate(selectedUid, firstBlock, defaultKeys[sector])) continue;
      authenticatedOnce = true;
      sectorComplete = true;
      for (uint8_t block = firstBlock; block < firstBlock + 4; ++block) {
        if (!pn5180ReadBlock(block, dumpData[block])) {
          failedBlock = block;
          sectorComplete = false;
          break;
        }
        blockValid[block] = true;
      }
    }
    if (!sectorComplete) {
      if (!selectedOnce) {
        error = "PN5180 preflight lost the selected tag before sector " + String(sector) +
                " after 3 attempts";
      } else if (!authenticatedOnce) {
        error = "PN5180 factory-key authentication failed in sector " + String(sector) +
                " after 3 attempts";
      } else {
        error = "PN5180 read failed at block " + String(failedBlock) +
                " after 3 sector attempts";
      }
      break;
    }
    completedSectors = sector + 1;
    jobMessage = "PN5180 read-only preflight: sector " +
                 String(completedSectors) + "/16";
    // Status requests remain available, while mutating endpoints reject the
    // active job through their normal conflict checks.
    server.handleClient();
    delay(2);
  }
  // The sector loop power-cycles the RF field before each selection, so HALT
  // is only needed after the final active exchange.
  reader.mifareHalt();

  memcpy(writableCandidateUid, cuidTestExpectedUid, sizeof(writableCandidateUid));
  writableCandidateAvailable = true;
  writableCandidateFactoryUid =
      memcmp(cuidTestExpectedUid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0;
  writableCandidateSectors = completedSectors;
  writableCandidateReady = writableCandidateFactoryUid && completedSectors == 16;
  tagAvailable = false;
  memcpy(autoTagUid, cuidTestExpectedUid, sizeof(autoTagUid));
  autoTagLatched = true;
  pn5180PresenceMisses = 0;

  if (completedSectors != 16) {
    if (error.isEmpty()) error = "PN5180 preflight did not complete all 16 sectors";
    return false;
  }
  const uint8_t expectedBcc = dumpData[0][0] ^ dumpData[0][1] ^
                              dumpData[0][2] ^ dumpData[0][3];
  if (memcmp(dumpData[0], cuidTestExpectedUid, 4) != 0 ||
      dumpData[0][4] != expectedBcc) {
    error = "PN5180 preflight manufacturer block UID/BCC validation failed";
    writableCandidateSectors = 0;
    return false;
  }
  jobMessage = "PN5180 preflight passed: 64/64 blocks read with 16/16 factory-key sectors";
  return true;
}

static bool writeCuidEngineeringTest(String &error) {
  static const uint8_t defaultKey[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  jobMessage = "Repeating full read-only preflight immediately before UID test…";
  if (!runCuidReadOnlyPreflight(error)) return false;
  uint8_t selectedUid[4] = {};
  if (!pn5180PrepareTag(cuidTestExpectedUid, selectedUid)) {
    error = "PN5180 could not reselect the expected CUID test tag";
    return false;
  }
  if (!pn5180Authenticate(selectedUid, 0, defaultKey)) {
    error = "PN5180 default-key authentication failed for block 0";
    return false;
  }
  uint8_t originalBlock0[BLOCK_SIZE] = {};
  if (!pn5180ReadBlock(0, originalBlock0) ||
      memcmp(originalBlock0, selectedUid, 4) != 0 ||
      originalBlock0[4] != (selectedUid[0] ^ selectedUid[1] ^
                            selectedUid[2] ^ selectedUid[3])) {
    error = "PN5180 preflight could not verify manufacturer block 0";
    return false;
  }

  uint8_t temporaryBlock0[BLOCK_SIZE];
  memcpy(temporaryBlock0, originalBlock0, BLOCK_SIZE);
  temporaryBlock0[3] ^= 0x01;
  temporaryBlock0[4] = temporaryBlock0[0] ^ temporaryBlock0[1] ^
                       temporaryBlock0[2] ^ temporaryBlock0[3];
  uint8_t temporaryUid[4];
  memcpy(temporaryUid, temporaryBlock0, sizeof(temporaryUid));

  jobMessage = "PN5180 writing temporary UID " + hexString(temporaryUid, 4) + "…";
  const bool temporaryWriteAck = pn5180WriteBlock(0, temporaryBlock0);

  delay(100);
  uint8_t verifiedTemporaryUid[4] = {};
  const bool temporarySelected =
      pn5180PrepareTag(temporaryUid, verifiedTemporaryUid);
  bool temporaryVerified = temporarySelected &&
                           pn5180Authenticate(verifiedTemporaryUid, 0, defaultKey);
  uint8_t temporaryReadback[BLOCK_SIZE] = {};
  temporaryVerified = temporaryVerified &&
      pn5180ReadBlock(0, temporaryReadback) &&
      memcmp(temporaryReadback, temporaryBlock0, BLOCK_SIZE) == 0;

  if (!temporarySelected) {
    uint8_t unchangedUid[4] = {};
    uint8_t unchangedBlock0[BLOCK_SIZE] = {};
    const bool originalVerified =
        pn5180PrepareTag(selectedUid, unchangedUid) &&
        pn5180Authenticate(unchangedUid, 0, defaultKey) &&
        pn5180ReadBlock(0, unchangedBlock0) &&
        memcmp(unchangedBlock0, originalBlock0, BLOCK_SIZE) == 0;
    error = originalVerified
        ? "Temporary UID write was rejected; original manufacturer block verified unchanged"
        : "URGENT: temporary write result is unknown and neither UID could be verified";
    return false;
  }

  jobMessage = "Temporary UID verified; restoring " + hexString(selectedUid, 4) + "…";
  if (!pn5180RestoreBlock0(temporaryUid, selectedUid, originalBlock0, defaultKey)) {
    error = "URGENT: temporary UID was written, but automatic restoration could not be verified";
    return false;
  }
  if (!temporaryVerified) {
    error = "Temporary write verification was incomplete; original manufacturer block was restored";
    return false;
  }
  if (!temporaryWriteAck) {
    error = "Temporary write had no ACK, but it occurred and the original manufacturer block was restored";
    return false;
  }
  memcpy(writableCandidateUid, selectedUid, sizeof(writableCandidateUid));
  memcpy(autoTagUid, selectedUid, sizeof(autoTagUid));
  autoTagLatched = true;
  jobMessage = "PN5180 CUID UID rewrite verified (" + hexString(selectedUid, 4) +
               " → " + hexString(temporaryUid, 4) + " → restored)";
  return true;
}
#else
static bool writeExactFuid(String &error) {
  error = "FUID writing is implemented only for the PN532 build"; return false;
}
static bool writeCuidEngineeringTest(String &error) {
  error = "CUID engineering writes are implemented only for the PN532 build";
  return false;
}
#endif

#if RFID_READER != READER_PN5180
static bool runCuidReadOnlyPreflight(String &error) {
  error = "Full CUID preflight is implemented only for the PN5180 build";
  return false;
}
#endif

static void sendJson(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

static void handleStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const bool unsupportedTagPresent = unsupportedUidAvailable &&
      millis() - unsupportedUidLastSeenAt < TAG_REMOVAL_MS;
  const bool decodedTagPresent = tagAvailable && autoTagLatched &&
                                 memcmp(lastUid, autoTagUid, sizeof(lastUid)) == 0;
  const bool writableCandidatePresent = writableCandidateAvailable && autoTagLatched &&
                                        memcmp(writableCandidateUid, autoTagUid,
                                               sizeof(writableCandidateUid)) == 0;
  const bool tagPresent = decodedTagPresent || writableCandidatePresent ||
                          unsupportedTagPresent;
  const bool readerArmed = readerReady && (jobType == JobType::Scan ||
                           (jobType == JobType::Idle && autoScanEnabled &&
                            !autoTagLatched && !unsupportedTagPresent));
  String json = "{\"ok\":" + String((connected && readerReady) ? "true" : "false");
  const String statusMessage = !readerReady ? "RFID reader unavailable; check wiring and power" :
      (connected ? "Reader ready" : "Connect Wi-Fi to browse the online library");
  json += ",\"message\":\"" + jsonEscape(statusMessage) + "\"";
#if RFID_READER == READER_RC522
  json += ",\"reader\":\"RC522\"";
#elif RFID_READER == READER_PN532
  json += ",\"reader\":\"PN532\"";
#else
  json += ",\"reader\":\"PN5180\"";
#endif
  json += ",\"readerReady\":" + String(readerReady ? "true" : "false");
  json += ",\"station\":\"" + jsonEscape(connected ? WiFi.SSID() : "disconnected") + "\"";
  json += ",\"ip\":\"" + jsonEscape(connected ? WiFi.localIP().toString() : "—") + "\"";
  json += ",\"ap\":\"" + jsonEscape(accessPointName) + "\"";
  json += ",\"busy\":" + String(jobType != JobType::Idle ? "true" : "false");
  json += ",\"jobOk\":" + String(jobSucceeded ? "true" : "false");
  json += ",\"job\":\"" + jsonEscape(jobMessage) + "\"";
  json += ",\"autoScan\":" + String(autoScanEnabled ? "true" : "false");
  json += ",\"readerArmed\":" + String(readerArmed ? "true" : "false");
  json += ",\"tagDetected\":" +
          String((autoTagLatched || unsupportedTagPresent) ? "true" : "false");
  if (autoTagLatched) json += ",\"detectedUid\":\"" + hexString(autoTagUid, 4) + "\"";
  json += ",\"tagPresent\":" + String(tagPresent ? "true" : "false");
  json += ",\"dumpLoaded\":" + String(libraryDumpLoaded ? "true" : "false");
  if (libraryDumpLoaded) {
    json += ",\"dumpName\":\"" + jsonEscape(libraryDumpName) + "\"";
    json += ",\"dumpUid\":\"" + hexString(libraryDump, 4) + "\"";
  }
  if (tagAvailable) json += ",\"tag\":" + decodedTagJson(&dumpData[0][0], lastUid, blockValid);
  if (unsupportedUidAvailable) {
    json += ",\"unsupportedTag\":{\"uid\":\"" +
            hexString(unsupportedUid, unsupportedUidLength) + "\"";
    json += ",\"uidLength\":" + String(unsupportedUidLength);
    json += ",\"present\":" + String(unsupportedTagPresent ? "true" : "false");
    json += ",\"reason\":\"Only four-byte UID MIFARE Classic 1K tags are supported\"}";
  }
  if (writableCandidateAvailable) {
    const bool likelyCuid = !writableCandidateFactoryUid &&
                            writableCandidateSectors == 16;
    json += ",\"writableTarget\":{\"uid\":\"" + hexString(writableCandidateUid, 4) + "\"";
    json += ",\"ready\":" + String(writableCandidateReady ? "true" : "false");
    json += ",\"factoryUidMatches\":" +
            String(writableCandidateFactoryUid ? "true" : "false");
    json += ",\"likelyCuid\":" + String(likelyCuid ? "true" : "false");
    json += ",\"likelyRewritable\":" + String(likelyCuid ? "true" : "false");
    json += ",\"bambuCompatible\":false";
    json += ",\"authenticatedSectors\":" + String(writableCandidateSectors);
    json += ",\"expectedSectors\":16";
    json += ",\"classification\":\"" + String(writableCandidateReady
        ? "Unused FUID candidate"
        : (likelyCuid
            ? "Likely CUID / rewritable tag"
            : "Unsupported or locked MIFARE Classic tag")) + "\"";
    json += ",\"status\":\"" + String(writableCandidateReady
        ? "Eligible candidate for the guarded FUID clone flow"
        : (likelyCuid
            ? "Rewritable, but not compatible with stock Bambu AMS/AMS Lite"
            : "Not eligible for the FUID write flow")) + "\"}";
  }
  json += "}";
  sendJson(200, json);
}

static void configureWebServer() {
  const char *collectedHeaders[] = {"X-OTA-Password"};
  server.collectHeaders(collectedHeaders, 1);
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", WEB_UI);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_POST, []() {
    if (!readerReady) { sendJson(503, "{\"message\":\"RFID reader is unavailable; check wiring and power\"}"); return; }
    if (jobType != JobType::Idle) { sendJson(409, "{\"message\":\"Another reader operation is active\"}"); return; }
    jobType = JobType::Scan; jobDeadline = millis() + JOB_TIMEOUT_MS;
    jobMessage = "Waiting for an ISO14443A tag…"; jobSucceeded = false;
    sendJson(202, "{\"ok\":true}");
  });
  server.on("/api/autoscan", HTTP_POST, []() {
    if (jobType != JobType::Idle) {
      sendJson(409, "{\"message\":\"Reader is busy\"}"); return;
    }
    autoScanEnabled = server.arg("enabled") == "1";
    autoTagLatched = false;
    autoNextScanAt = 0;
    preferences.putBool("autoscan", autoScanEnabled);
    jobMessage = autoScanEnabled ? "Auto-scan enabled; present a tag" : "Auto-scan paused";
    jobSucceeded = false;
    sendJson(200, "{\"ok\":true}");
  });
  server.on("/api/library/load", HTTP_POST, []() {
    if (jobType != JobType::Idle) { sendJson(409, "{\"message\":\"Reader is busy\"}"); return; }
    String error;
    if (!downloadLibraryDump(server.arg("path"), error)) {
      libraryDumpLoaded = false;
      sendJson(400, "{\"message\":\"" + jsonEscape(error) + "\"}"); return;
    }
    sendJson(200, "{\"ok\":true,\"name\":\"" + jsonEscape(libraryDumpName) +
                      "\",\"uid\":\"" + hexString(libraryDump, 4) + "\"}");
  });
  server.on("/api/write", HTTP_POST, []() {
    if (jobType != JobType::Idle) { sendJson(409, "{\"message\":\"Another reader operation is active\"}"); return; }
    if (!libraryDumpLoaded || server.arg("confirm") != "WRITE") {
      sendJson(400, "{\"message\":\"Load a dump and confirm WRITE first\"}"); return;
    }
    jobType = JobType::Write; jobDeadline = millis() + JOB_TIMEOUT_MS;
    jobMessage = "Write queued; keep exactly one verified FUID still on the reader";
    jobSucceeded = false;
    sendJson(202, "{\"ok\":true}");
  });
  server.on("/api/cuid-uid-test", HTTP_POST, []() {
    if (jobType != JobType::Idle) {
      sendJson(409, "{\"message\":\"Another reader operation is active\"}"); return;
    }
    if (server.arg("confirm") != "TEST UID") {
      sendJson(400, "{\"message\":\"Confirm TEST UID first\"}"); return;
    }
    if (!writableCandidateAvailable || writableCandidateFactoryUid ||
        writableCandidateSectors != 16) {
      sendJson(409, "{\"message\":\"Present one detected 16/16 non-FUID CUID candidate\"}");
      return;
    }
    memcpy(cuidTestExpectedUid, writableCandidateUid, sizeof(cuidTestExpectedUid));
    jobType = JobType::CuidTest;
    jobDeadline = millis() + JOB_TIMEOUT_MS;
    jobMessage = "CUID UID rewrite test queued; keep the test tag still";
    jobSucceeded = false;
    sendJson(202, "{\"ok\":true}");
  });
  server.on("/api/cuid-preflight", HTTP_POST, []() {
    if (jobType != JobType::Idle) {
      sendJson(409, "{\"message\":\"Another reader operation is active\"}"); return;
    }
#if RFID_READER != READER_PN5180
    sendJson(400, "{\"message\":\"Full CUID preflight is available only in the PN5180 build\"}");
    return;
#else
    if (!writableCandidateAvailable || !autoTagLatched ||
        memcmp(writableCandidateUid, autoTagUid, sizeof(autoTagUid)) != 0) {
      sendJson(409, "{\"message\":\"Present one detected four-byte test tag first\"}");
      return;
    }
    memcpy(cuidTestExpectedUid, writableCandidateUid, sizeof(cuidTestExpectedUid));
    jobType = JobType::CuidPreflight;
    jobDeadline = millis() + 60000;
    jobMessage = "Full PN5180 read-only preflight queued; keep exactly one tag still";
    jobSucceeded = false;
    sendJson(202, "{\"ok\":true}");
#endif
  });
  server.on("/api/dump", HTTP_GET, []() {
    if (!tagAvailable) { server.send(404, "text/plain", "No complete dump is available"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=bambu-" + hexString(lastUid, 4) + ".bin");
    server.send_P(200, "application/octet-stream",
                  reinterpret_cast<const char *>(&dumpData[0][0]), DUMP_SIZE);
  });
  server.on("/api/wifi", HTTP_POST, []() {
    const String ssid = server.arg("ssid");
    if (ssid.isEmpty() || ssid.length() > 32 || server.arg("password").length() > 63) {
      sendJson(400, "{\"message\":\"Invalid Wi-Fi credentials\"}"); return;
    }
    preferences.putString("ssid", ssid);
    preferences.putString("password", server.arg("password"));
    sendJson(200, "{\"ok\":true}");
    delay(250); ESP.restart();
  });
  server.on(
      "/api/firmware", HTTP_POST,
      []() {
        if (!firmwareUploadAuthorized) {
          sendJson(401, "{\"message\":\"Invalid OTA password or reader is busy\"}");
          return;
        }
        if (firmwareUploadFailed || Update.hasError()) {
          sendJson(500, "{\"message\":\"" + jsonEscape(firmwareUploadError) + "\"}");
          return;
        }
        sendJson(200, "{\"ok\":true,\"message\":\"Firmware installed; restarting\"}");
        delay(350);
        ESP.restart();
      },
      []() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          firmwareUploadAuthorized =
              jobType == JobType::Idle &&
              server.header("X-OTA-Password") == RFID_OTA_PASSWORD;
          firmwareUploadFailed = false;
          firmwareUploadError = "Firmware update failed";
          if (!firmwareUploadAuthorized) return;
          if (!upload.filename.endsWith(".bin")) {
            firmwareUploadFailed = true;
            firmwareUploadError = "Select a PlatformIO firmware.bin file";
            return;
          }
          Serial.print(F("Browser OTA start: "));
          Serial.println(upload.filename);
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            firmwareUploadFailed = true;
            firmwareUploadError = "ESP32 could not begin the flash update";
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (!firmwareUploadAuthorized || firmwareUploadFailed) return;
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            firmwareUploadFailed = true;
            firmwareUploadError = "Flash write failed during upload";
            Update.printError(Serial);
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (!firmwareUploadAuthorized || firmwareUploadFailed) return;
          if (!Update.end(true) || !Update.isFinished()) {
            firmwareUploadFailed = true;
            firmwareUploadError = "Uploaded image did not pass ESP32 validation";
            Update.printError(Serial);
          } else {
            Serial.print(F("Browser OTA complete, bytes: "));
            Serial.println(upload.totalSize);
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          firmwareUploadFailed = true;
          firmwareUploadError = "Firmware upload was aborted";
          Update.abort();
        }
      });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

static void setupArduinoOta() {
  if (WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname(RFID_HOSTNAME);
  ArduinoOTA.setPassword(RFID_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println(F("ArduinoOTA update started; do not remove power."));
  });
  ArduinoOTA.onEnd([]() { Serial.println(F("\nArduinoOTA update complete.")); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const unsigned int percent = total ? (progress / (total / 100U + 1U)) : 0;
    Serial.printf("\rArduinoOTA: %u%%", min(percent, 100U));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("\nArduinoOTA error %u\n", error);
  });
  ArduinoOTA.begin();
  Serial.println(F("ArduinoOTA ready as " RFID_HOSTNAME ".local"));
}

static void setupNetwork() {
  WiFi.mode(WIFI_AP_STA);
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xffffff);
  char apName[32];
  snprintf(apName, sizeof(apName), "Bambu-RFID-%06lX", static_cast<unsigned long>(suffix));
  accessPointName = apName;
  WiFi.softAP(accessPointName.c_str(), RFID_AP_PASSWORD);
  preferences.begin("bambu-rfid", false);
  autoScanEnabled = preferences.getBool("autoscan", true);
  String ssid = preferences.getString("ssid", WIFI_SSID);
  String password = preferences.getString("password", WIFI_PASSWORD);
  if (!ssid.isEmpty()) {
    WiFi.setHostname(RFID_HOSTNAME);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.print(F("Connecting to Wi-Fi"));
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) {
      delay(250); Serial.print('.');
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin(RFID_HOSTNAME);
    Serial.print(F("Web UI: http://" RFID_HOSTNAME ".local/ or http://"));
    Serial.println(WiFi.localIP());
  }
  Serial.print(F("Setup AP: ")); Serial.print(accessPointName);
  Serial.print(F(" / password: ")); Serial.println(RFID_AP_PASSWORD);
  Serial.println(F("Setup UI: http://192.168.4.1/"));
}

void setup() {
  Serial.begin(115200);
  delay(400);
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
#if RFID_READER == READER_RC522
  reader.PCD_Init();
  Serial.println(F("Bambu RFID web reader ready (RC522; writes disabled)."));
#elif RFID_READER == READER_PN532
  reader.begin();
  if (!reader.getFirmwareVersion()) {
    Serial.println(F("PN532 not found. Check SPI mode jumpers and wiring."));
    while (true) delay(1000);
  }
  reader.SAMConfig();
  Serial.println(F("Bambu RFID web reader ready (PN532)."));
#else
  reader.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  // Long jumper wires are substantially more reliable below the PN5180's
  // 7 MHz host-interface limit; RF timing is unaffected by this SPI setting.
  reader.setSPISettingsFrecuency(500000);
  // PN5180 MFC_AUTHENTICATE may legitimately take close to one second. Keep
  // the wait bounded while allowing the command to finish on real hardware.
  reader.commandTimeout = 1200;
  reader.reset();
  uint8_t productVersion[2] = {0xff, 0xff};
  uint8_t firmwareVersion[2] = {0xff, 0xff};
  uint8_t eepromVersion[2] = {0xff, 0xff};
  const bool productRead = reader.readEEprom(PRODUCT_VERSION, productVersion,
                                              sizeof(productVersion));
  const bool firmwareRead = reader.readEEprom(FIRMWARE_VERSION, firmwareVersion,
                                               sizeof(firmwareVersion));
  const bool eepromRead = reader.readEEprom(EEPROM_VERSION, eepromVersion,
                                             sizeof(eepromVersion));
  readerReady = productRead && firmwareRead && eepromRead &&
                productVersion[1] != 0xff && reader.setupRF();
  if (readerReady) {
    Serial.print(F("PN5180 product/firmware/EEPROM: "));
    Serial.print(productVersion[1]); Serial.print('.'); Serial.print(productVersion[0]);
    Serial.print(F(" / "));
    Serial.print(firmwareVersion[1]); Serial.print('.'); Serial.print(firmwareVersion[0]);
    Serial.print(F(" / "));
    Serial.print(eepromVersion[1]); Serial.print('.'); Serial.println(eepromVersion[0]);
    Serial.println(F("Bambu RFID web reader ready (PN5180)."));
  } else {
    Serial.println(F("PN5180 initialization failed. Check 3.3 V/GND, SPI, NSS, BUSY, and RST."));
  }
#endif
  setupNetwork();
  setupArduinoOta();
  configureWebServer();
}

void loop() {
  server.handleClient();
  if (jobType == JobType::Idle) {
    ArduinoOTA.handle();
    serviceAutoScan();
    delay(2);
    return;
  }
  if (jobType == JobType::Scan) {
    uint8_t uid[4];
    if (detectTag(uid, 50)) {
      latchAutoTag(uid);
      jobSucceeded = readCurrentTag(uid);
      jobType = JobType::Idle;
    } else if (static_cast<int32_t>(millis() - jobDeadline) >= 0) {
      jobMessage = "Scan timed out; no supported four-byte UID tag found";
      jobSucceeded = false; jobType = JobType::Idle;
    }
    return;
  }
  if (jobType == JobType::Write) {
    String error;
    const bool success = writeExactFuid(error);
    jobSucceeded = success;
    jobMessage = success ? "Write and full 1 KiB verification completed" : error;
    jobType = JobType::Idle;
  }
  if (jobType == JobType::CuidPreflight) {
    String error;
    const bool success = runCuidReadOnlyPreflight(error);
    jobSucceeded = success;
    if (!success) jobMessage = error;
    jobType = JobType::Idle;
  }
  if (jobType == JobType::CuidTest) {
    String error;
    const bool success = writeCuidEngineeringTest(error);
    jobSucceeded = success;
    if (!success) jobMessage = error;
    jobType = JobType::Idle;
  }
}
