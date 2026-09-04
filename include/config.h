#pragma once

// Select exactly one reader. PN5180 is the primary tested target.
#define READER_RC522 1
#define READER_PN532 2
#define READER_PN5180 3
#ifndef RFID_READER
#define RFID_READER READER_PN5180
#endif

// Shared VSPI pins on a classic ESP32 DevKit/WROOM board.
#define RFID_SCK_PIN 18
#define RFID_MISO_PIN 19
#define RFID_MOSI_PIN 23

// RC522: SDA/SS/CS and RST. PN532 SPI: SS/CS only.
#define RFID_SS_PIN 5
#define RFID_RST_PIN 22

// Experimental PN5180 SPI control pins. GPIO2 is a boot strap pin on classic
// ESP32 boards; if booting becomes unreliable, verify that the module does not
// hold RST high or otherwise drive GPIO2 during ESP32 reset.
#define PN5180_BUSY_PIN 4
#define PN5180_RST_PIN 2

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
