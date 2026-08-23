#pragma once

// Select exactly one reader. PN532 is the default for this project.
#define READER_RC522 1
#define READER_PN532 2
#ifndef RFID_READER
#define RFID_READER READER_PN532
#endif

// Shared VSPI pins on a classic ESP32 DevKit/WROOM board.
#define RFID_SCK_PIN 18
#define RFID_MISO_PIN 19
#define RFID_MOSI_PIN 23

// RC522: SDA/SS/CS and RST. PN532 SPI: SS/CS only.
#define RFID_SS_PIN 5
#define RFID_RST_PIN 22

// The device always starts a protected setup/access-point so it remains
// reachable even when the configured Wi-Fi network is unavailable.
#define RFID_HOSTNAME "bambu-rfid"
#define RFID_AP_PASSWORD "bambu-rfid"

// Protects both browser firmware uploads and ArduinoOTA. Change this before
// exposing the device on an untrusted network.
#ifndef RFID_OTA_PASSWORD
#define RFID_OTA_PASSWORD "bambu-rfid"
#endif

// Optional compile-time station credentials. Leave empty to configure Wi-Fi
// from the web UI at http://192.168.4.1/. Saved web credentials take priority.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
