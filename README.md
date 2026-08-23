# ESP32 Bambu RFID Workbench

This firmware turns an ESP32 and PN532 into a local web-based Bambu Lab RFID
reader and guarded FUID clone writer. It derives Bambu's public per-sector keys,
decodes genuine filament tags, downloads exact signed dumps from the
[Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library),
and writes compatible unused FUID tags.

The web UI provides:

- timed PN532 scans and decoded filament information;
- automatic periodic scans that read each presented UID once and re-arm after
  tag removal, with a persisted UI enable/disable control;
- download of a complete locally read 1 KiB dump;
- live material/color/UID navigation of the GitHub tag library;
- ESP32-side HTTPS download and validation of selected 1,024-byte dumps;
- guarded, explicitly confirmed FUID writing followed by full readable-content
  verification;
- Wi-Fi provisioning stored in ESP32 Preferences/NVS.

## Important write limitations

Changing a material, color, or any other signed field invalidates Bambu's
RSA-2048 signature. This firmware therefore clones complete, unmodified signed
dumps; it does not generate arbitrary genuine-looking filament records.

Writing currently accepts only an unused FUID with factory UID `AA55C396`. It
refuses all other UIDs. Block 0 is written last, after the other 63 blocks have
succeeded, because changing the FUID is irreversible. Do not remove or move the
tag during a write.

UFUID is not written or sealed. Its Gen1/Gen4 magic wake-up and sealing sequence
requires raw seven-bit commands that the standard Adafruit PN532 API does not
reliably expose. Use a Proxmark3 and the upstream
[write guide](https://github.com/queengooborg/Bambu-Lab-RFID-Tag-Guide/blob/master/docs/WriteTags.md)
for those tags. CUID/Gen2 and Gen1 tags are deliberately unsupported because
the AMS rejects or damages them.

## Wiring: PN532 in SPI mode

Set the PN532 module's jumpers or DIP switches to **SPI mode** first. Switch
positions vary by board.

| PN532 | ESP32 DevKit |
|---|---:|
| 3.3V | 3.3V |
| GND | GND |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| SS / SSEL / CS | GPIO 5 |

Use 3.3 V logic and keep one label centered over the antenna. Pin definitions
are in `include/config.h`.

## Build and upload

```sh
cd esp32-bambu-rfid-reader
pio run -e pn532 -t upload
pio device monitor -b 115200
```

The optional `rc522` environment still builds and supports reading, but the
write endpoint reports that writing requires the PN532 build.

## Firmware updates over Wi-Fi

Two password-protected OTA methods are available after the ESP32 joins the home
network. The default OTA password is `bambu-rfid`; change
`RFID_OTA_PASSWORD` in `include/config.h` before using the device on an
untrusted network.

### Web browser

1. Build the firmware without uploading it:

   ```sh
   pio run -e pn532
   ```

2. Open `http://bambu-rfid.local/`.
3. In **Firmware update over Wi-Fi**, choose
   `.pio/build/pn532/firmware.bin`.
4. Enter the OTA password and confirm the update.
5. Keep the ESP32 powered until it validates the image and restarts.

The endpoint rejects updates while an RFID operation is active and uses the
ESP32 Update API to validate the uploaded application image before rebooting.

### PlatformIO / ArduinoOTA

The `pn532-ota` environment targets `bambu-rfid.local`:

```sh
pio run -e pn532-ota -t upload
```

If mDNS is unavailable, replace `bambu-rfid.local` in `platformio.ini` with the
station IP shown in the web UI. Keep the `--auth` value in `platformio.ini`
synchronized with `RFID_OTA_PASSWORD` if the default is changed.

## First connection

1. After boot, connect to the `Bambu-RFID-XXXXXX` Wi-Fi network.
2. Use password `bambu-rfid`.
3. Open `http://192.168.4.1/` and save the home Wi-Fi credentials.
4. After restart, reconnect the phone/computer to the home network.
5. Open `http://bambu-rfid.local/` or use the IP printed at 115200 baud.

The protected setup AP remains enabled as a recovery path. Saved credentials
take priority over optional compile-time `WIFI_SSID` and `WIFI_PASSWORD` values
in `config.h`.

The browser uses GitHub's public Contents API to navigate the catalog, and the
ESP32 downloads the selected binary from GitHub. GitHub may rate-limit heavy
anonymous browsing. No GitHub token is stored on the device.

## Safe workflow

1. Read a genuine tag first and confirm PN532 positioning is reliable.
2. Browse to a library material, color, UID, and select its `-dump.bin` file.
3. Confirm the displayed source UID.
4. Remove every other tag from the PN532 antenna.
5. Place one new FUID showing factory UID `AA55C396`.
6. Type `WRITE`, accept the final confirmation, and keep the tag still until
   the UI reports successful verification.

Writing RFID tags is experimental and can permanently consume or lock a tag.
There is no warranty that a marketplace product matches its advertised chip
generation.
