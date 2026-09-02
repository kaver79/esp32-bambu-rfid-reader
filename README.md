# ESP32 Bambu RFID Workbench

This firmware turns an ESP32 and PN532 into a local web-based Bambu Lab RFID
reader and research workbench. It derives Bambu's public per-sector keys,
decodes genuine filament tags, and browses exact signed dumps from the
[Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library).

## Project status

**Read-only mode is the current working mode.** Reading, automatic tag
detection, decoded spool information, GitHub library enrichment, and dump
downloads are working well on the PN532 setup used for this project.

**The RFID write flow is not finished.** The repository contains experimental
write code and UI scaffolding, but it has not completed end-to-end hardware and
Bambu Lab printer validation. Do not rely on it to create usable tags, and do
not test it with a tag that you cannot afford to permanently consume.

The web UI provides:

- timed PN532 scans and decoded filament information;
- automatic periodic scans that read each presented UID once and re-arm after
  tag removal, with a persisted UI enable/disable control and clear scanning,
  tag-detected, and last-scan indicators, plus an animated clockwise rotation
  guide while the reader is searching;
- automatic GitHub library enrichment showing exact-UID or variant matches,
  official material and color names, filament codes, availability, and a
  direct source link;
- download of a complete locally read 1 KiB dump;
- read-only inspection of blank FUID candidates: the UI reports eligibility
  only when the factory UID is `AA55C396` and all 16 sectors authenticate with
  the default factory key;
- detection details for other four-byte UID tags, including their UID and
  default-key sector count, while keeping them ineligible for writing;
- safe detection and UI reporting for ISO14443A tags with non-four-byte UIDs,
  without passing a too-small UID buffer to the PN532 library;
- live material/color/UID navigation of the GitHub tag library;
- ESP32-side HTTPS download and validation of selected 1,024-byte dumps;
- unfinished experimental FUID write scaffolding, which is not currently a
  supported workflow;
- Wi-Fi provisioning stored in ESP32 Preferences/NVS.

## Write flow status and limitations

The write controls are experimental and the write workflow is not considered
complete or ready for normal use. The descriptions below document the intended
safety boundaries; they are not a claim that writing has been validated.

Changing a material, color, or any other signed field invalidates Bambu's
RSA-2048 signature. This firmware therefore clones complete, unmodified signed
dumps; it does not generate arbitrary genuine-looking filament records.

Writing currently accepts only an unused FUID with factory UID `AA55C396`. It
refuses all other UIDs. Block 0 is written last, after the other 63 blocks have
succeeded, because changing the FUID is irreversible. Do not remove or move the
tag during a write.

When such a factory UID is scanned, the firmware performs a read-only
eligibility inspection and shows how many of the 16 sectors accept the default
factory key. A `16/16` result means only that the tag is a candidate for the
unfinished FUID flow. It does not prove that the chip supports the required
magic block-0 behavior, that a write will succeed, or that an AMS/AMS Lite will
accept the result.

Other factory-keyed MIFARE Classic tags are still shown in the UI so an
unsupported card is not mistaken for an antenna failure. A non-FUID UID with
`16/16` default-key sectors is labeled **Likely CUID / rewritable** and clearly
marked incompatible with stock AMS/AMS Lite. This is a conservative inference,
not proof of the magic generation, and the write flow remains disabled. Use a
Proxmark3 `hf mf info` test to distinguish CUID, Gen1, Gen4, and other magic
capabilities without guessing.

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

Very small or bare sticker/coin antennas may detune when pressed directly
against the PN532 board. If a known 13.56 MHz ISO14443A tag produces no UID at
all, try a non-metallic 5–10 mm spacer and rotate the tag slowly. The PN532
firmware cannot detect 125 kHz LF tags, UHF tags, or non-ISO14443A NFC protocols.

## Build and upload

```sh
cd esp32-bambu-rfid-reader
pio run -e pn532 -t upload
pio device monitor -b 115200
```

The optional `rc522` environment still builds and supports reading. Writing
remains unfinished and unsupported on all builds.

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

## Read-only workflow

1. Open the web UI and leave **Auto-scan** enabled.
2. Keep the spool face flat over the PN532 and rotate it slowly as indicated.
3. Stop when the UI reports that a tag was detected.
4. Review the decoded tag and GitHub library information.
5. Download the complete 1 KiB dump if needed.
6. Remove the spool; the UI keeps the result clearly marked as **Last scan**.

Writing RFID tags is still under development and can permanently consume or
lock a tag. There is no warranty that a marketplace product matches its
advertised chip generation.

## Credits

- [Bambu Research Group RFID Tag Guide](https://github.com/Bambu-Research-Group/RFID-Tag-Guide)
  for documenting the Bambu Lab RFID format, key derivation, and tag research.
- [Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library)
  and its contributors for collecting and organizing community tag dumps and
  filament metadata.
- [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532) for the PN532
  Arduino library used by the primary reader build.
- [MFRC522](https://github.com/miguelbalboa/rfid) for optional RC522 read-only
  compatibility.
- The Arduino, Espressif, and PlatformIO communities for the ESP32 framework,
  networking, OTA, and build tooling used by this project.
