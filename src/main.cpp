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
#else
#error "RFID_READER must be READER_RC522 or READER_PN532"
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

enum class JobType { Idle, Scan, Write };
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
static bool writableCandidateAvailable = false;
static bool writableCandidateReady = false;
static bool writableCandidateFactoryUid = false;
static uint8_t writableCandidateUid[4];
static uint8_t writableCandidateSectors = 0;

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

#if RFID_READER == READER_RC522
static bool detectTag(uint8_t uid[4], uint16_t timeoutMs = 50) {
  (void)timeoutMs;
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return false;
  if (reader.uid.size != 4) { reader.PICC_HaltA(); return false; }
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
#else
static bool detectTag(uint8_t uid[4], uint16_t timeoutMs = 50) {
  uint8_t uidLength = 0;
  if (!reader.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, timeoutMs)) return false;
  return uidLength == 4;
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
#endif

// This is intentionally read-only. The factory UID and default keys make a tag
// eligible for the unfinished FUID flow; they do not prove its magic commands
// or compatibility with an AMS/AMS Lite.
static bool inspectBlankTarget(const uint8_t uid[4]) {
  static uint8_t defaultKey[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t authenticatedSectors = 0;
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
#else
  for (uint8_t sector = 0; sector < 16; ++sector) {
    if (reader.mifareclassic_AuthenticateBlock(const_cast<uint8_t *>(uid), 4,
                                                sector * 4, 0, defaultKey)) {
      ++authenticatedSectors;
    }
  }
#endif
  memcpy(writableCandidateUid, uid, sizeof(writableCandidateUid));
  writableCandidateAvailable = true;
  writableCandidateSectors = authenticatedSectors;
  writableCandidateFactoryUid =
      memcmp(uid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0;
  writableCandidateReady = writableCandidateFactoryUid && authenticatedSectors == 16;
  tagAvailable = false;
  if (writableCandidateReady) {
    jobMessage = "Blank FUID candidate: all 16 sectors accept the factory key";
  } else if (authenticatedSectors == 16) {
    jobMessage = "Factory-keyed tag detected, but UID " + hexString(uid, 4) +
                 " is not the supported FUID factory UID AA55C396";
  } else {
    jobMessage = "UID " + hexString(uid, 4) + ": " +
                 String(authenticatedSectors) +
                 "/16 sectors accept the factory key; write eligibility not established";
  }
  Serial.print(F("Blank target inspection: UID "));
  printHex(uid, 4, 0);
  Serial.print(F(", default-key sectors "));
  Serial.print(authenticatedSectors);
  Serial.println(F("/16 (read-only inspection)"));
  return writableCandidateReady;
}

static bool readCurrentTag(const uint8_t uid[4]) {
  if (memcmp(uid, FUID_FACTORY_UID, sizeof(FUID_FACTORY_UID)) == 0) {
    return inspectBlankTarget(uid);
  }
  writableCandidateAvailable = false;
  writableCandidateReady = false;
  writableCandidateFactoryUid = false;
  writableCandidateSectors = 0;
  uint8_t keys[16][6];
  if (!deriveBambuKeyA(uid, keys)) { jobMessage = "Key derivation failed"; return false; }
  const size_t blocks = readAllBlocks(keys, uid);
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
}

static void serviceAutoScan() {
  if (!autoScanEnabled || jobType != JobType::Idle) return;
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

  uint8_t sourceKeys[16][6];
  if (!deriveBambuKeyA(libraryDump, sourceKeys)) {
    error = "Could not derive the source key for the final UID write"; return false;
  }
  if (!reader.mifareclassic_AuthenticateBlock(targetUid, 4, 0, 0, sourceKeys[0])) {
    error = "Could not re-authenticate sector 0 before the final UID write"; return false;
  }
  if (!reader.mifareclassic_WriteDataBlock(0, libraryDump)) {
    error = "Final UID write failed; inspect this tag before reusing it"; return false;
  }

  delay(150);
  reader.SAMConfig();
  uint8_t newUid[4] = {};
  if (!detectTag(newUid, 800) || memcmp(newUid, libraryDump, 4) != 0) {
    error = "Write completed but the new UID could not be verified"; return false;
  }
  uint8_t keys[16][6];
  if (!deriveBambuKeyA(newUid, keys) || readAllBlocks(keys, newUid) != BLOCK_COUNT ||
      !readableContentMatchesLibrary()) {
    error = "UID changed, but full 1 KiB verification failed"; return false;
  }
  memcpy(lastUid, newUid, 4);
  tagAvailable = true;
  latchAutoTag(newUid);
  return true;
}
#else
static bool writeExactFuid(String &error) {
  error = "FUID writing is implemented only for the PN532 build"; return false;
}
#endif

static void sendJson(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

static void handleStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const bool decodedTagPresent = tagAvailable && autoTagLatched &&
                                 memcmp(lastUid, autoTagUid, sizeof(lastUid)) == 0;
  const bool writableCandidatePresent = writableCandidateAvailable && autoTagLatched &&
                                        memcmp(writableCandidateUid, autoTagUid,
                                               sizeof(writableCandidateUid)) == 0;
  const bool tagPresent = decodedTagPresent || writableCandidatePresent;
  const bool readerArmed = jobType == JobType::Scan ||
                           (jobType == JobType::Idle && autoScanEnabled &&
                            !autoTagLatched);
  String json = "{\"ok\":" + String(connected ? "true" : "false");
  json += ",\"message\":\"" + jsonEscape(connected ? "Reader ready" : "Connect Wi-Fi to browse the online library") + "\"";
  json += ",\"station\":\"" + jsonEscape(connected ? WiFi.SSID() : "disconnected") + "\"";
  json += ",\"ip\":\"" + jsonEscape(connected ? WiFi.localIP().toString() : "—") + "\"";
  json += ",\"ap\":\"" + jsonEscape(accessPointName) + "\"";
  json += ",\"busy\":" + String(jobType != JobType::Idle ? "true" : "false");
  json += ",\"jobOk\":" + String(jobSucceeded ? "true" : "false");
  json += ",\"job\":\"" + jsonEscape(jobMessage) + "\"";
  json += ",\"autoScan\":" + String(autoScanEnabled ? "true" : "false");
  json += ",\"readerArmed\":" + String(readerArmed ? "true" : "false");
  json += ",\"tagDetected\":" + String(autoTagLatched ? "true" : "false");
  if (autoTagLatched) json += ",\"detectedUid\":\"" + hexString(autoTagUid, 4) + "\"";
  json += ",\"tagPresent\":" + String(tagPresent ? "true" : "false");
  json += ",\"dumpLoaded\":" + String(libraryDumpLoaded ? "true" : "false");
  if (libraryDumpLoaded) {
    json += ",\"dumpName\":\"" + jsonEscape(libraryDumpName) + "\"";
    json += ",\"dumpUid\":\"" + hexString(libraryDump, 4) + "\"";
  }
  if (tagAvailable) json += ",\"tag\":" + decodedTagJson(&dumpData[0][0], lastUid, blockValid);
  if (writableCandidateAvailable) {
    json += ",\"writableTarget\":{\"uid\":\"" + hexString(writableCandidateUid, 4) + "\"";
    json += ",\"ready\":" + String(writableCandidateReady ? "true" : "false");
    json += ",\"factoryUidMatches\":" +
            String(writableCandidateFactoryUid ? "true" : "false");
    json += ",\"authenticatedSectors\":" + String(writableCandidateSectors);
    json += ",\"expectedSectors\":16";
    json += ",\"classification\":\"" + String(writableCandidateReady
        ? "Unused FUID candidate"
        : (writableCandidateSectors == 16
            ? "Factory-keyed MIFARE Classic tag"
            : "Unsupported or locked MIFARE Classic tag")) + "\"";
    json += ",\"status\":\"" + String(writableCandidateReady
        ? "Eligible candidate for the unfinished write flow"
        : "Not eligible for the FUID write flow") + "\"}";
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
    if (jobType != JobType::Idle) { sendJson(409, "{\"message\":\"Another reader operation is active\"}"); return; }
    jobType = JobType::Scan; jobDeadline = millis() + JOB_TIMEOUT_MS;
    jobMessage = "Waiting for a Bambu tag…"; jobSucceeded = false;
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
    jobMessage = "Write queued; place one unused FUID on the PN532"; jobSucceeded = false;
    sendJson(202, "{\"ok\":true}");
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
#else
  reader.begin();
  if (!reader.getFirmwareVersion()) {
    Serial.println(F("PN532 not found. Check SPI mode jumpers and wiring."));
    while (true) delay(1000);
  }
  reader.SAMConfig();
  Serial.println(F("Bambu RFID web reader ready (PN532)."));
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
}
