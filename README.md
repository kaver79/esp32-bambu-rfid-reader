# ESP32 Bambu RFID Workbench

This firmware turns an ESP32 and PN5180 into a local web-based Bambu Lab RFID
reader and research workbench. It derives Bambu's public per-sector keys,
decodes genuine filament tags, and browses exact signed dumps from the
[Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library).

New build? Follow the separate
**[Easy Install and FUID Tag Programming Guide](docs/EASY_INSTALL.md)** for the
complete PN5180 wiring, installation, validation, programming, recovery, and
AMS Lite setup flow.

## Project status

PN5180 is the primary hardware target. UID detection, CUID full reads, the
reversible CUID UID test, interrupted-write recovery, and exact FUID cloning
are hardware-verified. The resulting PLA Basic Black clone is also verified on
a stock Bambu Lab A1 Combo with AMS Lite, including an unload/reload cycle.

The application cannot generate Bambu signatures or safely edit signed fields.
It can only copy a complete validated signed dump. Do not test with a tag that
you cannot afford to permanently consume.

The web UI provides:

- timed PN5180 scans and tag inspection;
- automatic periodic scans that read each presented UID once and re-arm after
  tag removal, with a persisted UI enable/disable control and clear scanning,
  tag-detected, and last-scan indicators, plus an animated clockwise rotation
  guide while the reader is searching;
- automatic GitHub library enrichment showing exact-UID or variant matches,
  official material and color names, filament codes, availability, and a
  direct source link;
- download of a complete locally read 1 KiB dump;
- two-stage read-only inspection of blank FUID candidates: a fast block-0
  probe identifies factory UID `AA55C396` as requiring validation, and the
  dedicated **Validate FUID (read-only)** action reports eligibility only
  after all 16 sectors authenticate with the default factory key;
- detection details for other four-byte UID tags, including their UID and
  default-key sector count, while keeping them ineligible for writing;
- safe detection and UI reporting for ISO14443A tags with non-four-byte UIDs,
  without passing a too-small UID buffer to the reader library;
- live material/color/UID navigation of the GitHub tag library;
- ESP32-side HTTPS download and validation of selected 1,024-byte dumps;
- guarded PN5180 FUID cloning with block 0 last and complete readback;
- a separate reversible CUID UID-write test, explicitly labeled as unrelated
  to stock AMS/AMS Lite compatibility;
- Wi-Fi provisioning stored in ESP32 Preferences/NVS.

## Write flow status and limitations

The FUID clone path has completed a full hardware write, 1 KiB readback, and a
stock A1 Combo/AMS Lite recognition test. This result applies to the tested
FUID and exact signed dump; it does not make other magic-tag generations
compatible.

Changing a material, color, or any other signed field invalidates Bambu's
RSA-2048 signature. This firmware therefore clones complete, unmodified signed
dumps; it does not generate arbitrary genuine-looking filament records.

The Bambu-compatible write path accepts only an unused FUID with factory UID
`AA55C396`. It refuses all other UIDs. Block 0 is written last, after the other
63 blocks have succeeded, because changing the FUID is irreversible. Do not
remove or move the tag during a write.

If an interrupted attempt has already replaced every sector key but left block
0 at factory UID `AA55C396`, the same selected dump can be used for a guarded
recovery. Before retrying block 0, the firmware authenticates all sixteen
sectors with the selected dump's derived keys and verifies every readable byte
outside manufacturer block 0. Any mismatch aborts recovery without writing.

The normal PN5180 scan is intentionally fast and probes only block 0. For an
`AA55C396` tag, the initial `1/16` display therefore means **Possible unused
FUID — validation required**, not failure or incompatibility. Press **Validate
FUID (read-only)** while keeping the same tag still. Only a successful 64-block
read with `16/16` default-key sectors changes the state to **Unused FUID
candidate** and **Eligible for exact FUID cloning**.

These UI states have distinct meanings:

| UID and read result | UI meaning | FUID clone action |
|---|---|---|
| `AA55C396`, quick `1/16` probe | Possible unused FUID; full validation required | Run **Validate FUID (read-only)** |
| `AA55C396`, validated `16/16` | Unused FUID candidate | Eligible only when a signed dump is also loaded and the tag remains present |
| Non-factory UID, quick `1/16` probe | Possible rewritable tag; type not established | Optional **Validate CUID (read-only)** engineering check |
| Non-factory UID, validated `16/16` | Likely CUID/rewritable | Never eligible for the Bambu clone flow |
| Factory authentication fails | Unsupported, locked, or unstable read | Do not write |

A blank validated FUID is shown as **Blank tag — not a Bambu clone yet**. It is
not meaningful to call it AMS-compatible or incompatible before programming.
The application claims a completed clone only after the exact signed dump is
written, the new UID is reselected, and the complete readback matches.

A `16/16` result still means only that the tag is a candidate for the guarded
FUID flow. It does not prove that the chip supports the required magic block-0
behavior, that a write will succeed, or that an AMS/AMS Lite will accept the
result.

Other factory-keyed MIFARE Classic tags are still shown in the UI so an
unsupported card is not mistaken for an antenna failure. A non-FUID UID with
`16/16` default-key sectors is labeled **Likely CUID / rewritable** and clearly
marked incompatible with stock AMS/AMS Lite. This is a conservative inference,
not proof of the magic generation, and the Bambu-compatible write flow remains
disabled. Use a Proxmark3 `hf mf info` test to distinguish CUID, Gen1, Gen4, and
other magic capabilities without guessing.

For replaceable test media only, a separate **Reversible CUID UID test** checks
whether the PN5180 can modify block 0. It requires a currently detected non-FUID
four-byte UID with `16/16` default-key sectors, the exact confirmation `TEST
UID`, and a final browser warning. It first reads all 64 blocks, preserves the
manufacturer block, changes only UID and BCC to a temporary value, verifies the
temporary UID, restores the original manufacturer block, and verifies that
restoration byte-for-byte. Filament data and sector trailers are never written.
Passing this test does not establish stock AMS/AMS Lite compatibility.

UFUID is not written or sealed. Its Gen1/Gen4 magic wake-up and sealing sequence
requires raw seven-bit commands that the standard Adafruit PN532 API does not
reliably expose. Use a Proxmark3 and the upstream
[write guide](https://github.com/queengooborg/Bambu-Lab-RFID-Tag-Guide/blob/master/docs/WriteTags.md)
for those tags. CUID/Gen2 and Gen1 tags remain unsupported for printer use
because the AMS rejects or may damage them; the isolated UID capability test
does not change that compatibility status.

## Primary hardware: PN5180

PN5180 is the default, fully tested target and the only target that currently
supports the guarded FUID clone workflow.

| PN5180 | ESP32 DevKit |
|---|---:|
| 5V | 5V / VIN (RF/output-stage supply) |
| 3.3V / PVDD | 3V3 (logic supply/reference) |
| GND | GND |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |
| NSS / CS | GPIO 5 |
| BUSY | GPIO 4 |
| RST | GPIO 2 |

The commonly used PN5180 carrier requires both power rails: connect `5V` to
ESP32 `5V`/`VIN`, connect `3.3V`/`PVDD` to ESP32 `3V3`, and use a common ground.
SPI and control signals are 3.3 V logic. Check the exact module labels before
powering it; never connect 5 V to `PVDD` or an ESP32 GPIO. GPIO 2 is also an
ESP32 boot strap pin, so disconnect or move PN5180 RST if normal boot fails.

Build and upload the primary target:

```sh
cd esp32-bambu-rfid-reader
pio run -e pn5180 -t upload
pio device monitor -b 115200
```

Pin definitions are maintained in `include/config.h`. The optional `pn532` and
`rc522` environments remain available only for compatibility and read-focused
use.

## PN5180 behavior and validated results

The `pn5180` environment provides startup diagnostics, ISO14443A UID detection,
a bounded read-only block-0 default-key probe, the guarded reversible CUID UID
test, and exact FUID cloning. For factory UID `AA55C396`, the UI first reports
**Possible unused FUID — validation required** instead of treating the quick
`1/16` probe as a final compatibility result. The dedicated **Validate FUID
(read-only)** action reselects the same UID for every sector, authenticates all
16 sectors with Key
A `FFFFFFFFFFFF`, reads all 64 blocks, and validates the manufacturer UID/BCC.
Each sector gets a bounded reset/reselect/authentication retry, but any sector
that cannot be read completely still aborts the preflight. The UID-write test
remains disabled until that complete preflight succeeds. During FUID cloning,
each sector is written data-first and trailer-last, then reauthenticated with
the source key and verified before the next sector. Manufacturer block 0 is
written only after all 16 sectors pass, followed by a complete readback.

The maintained Elechouse Arduino fork bounds BUSY and reset waits. After the
initial UID and authentication result, the firmware leaves the tag halted and
uses a lightweight periodic WUPA check to distinguish a tag still on the
antenna from retained last-scan data. Three consecutive missed checks mark the
tag removed without repeating authentication or block reads. Hardware testing
with UID `61A8CC3F` authenticated all 16 sectors and read all 64 blocks using
the default `FFFFFFFFFFFF` Key A. A guarded test changed the UID to
`61A8CC3E` and issued the restoration to `61A8CC3F`. The PN5180 missed the
immediate restore-verification exchange, but a fresh explicit scan followed
by another complete CUID validation verified the original UID/BCC and all 16
sectors. A repeat test with a non-metallic spacer between the tag and antenna
completed cleanly end to end: temporary UID `61A8CC3E` was verified, original
UID `61A8CC3F` was restored and verified, and an independent final preflight
again read 64/64 blocks across 16/16 sectors. If UID detection works but block
reads are intermittent, try a 5–10 mm non-metallic tag-to-antenna gap.

The first PN5180 FUID test used the validated PLA Basic Black dump with source
UID `02034E6D`. Two deliberately interrupted attempts exercised mixed-state
recovery while manufacturer UID `AA55C396` remained unchanged. The completed
run recovered each already-personalized sector independently, wrote and
verified all remaining sectors, wrote block 0 last, reselected UID `02034E6D`,
and verified the complete 1,024-byte dump. The decoded readback reported PLA
Basic, black `#000000FF`, 1.750 mm, and 1000 g. A stock A1 Combo with AMS Lite
then recognized the clone as black PLA, and recognition survived a normal
unload/reload flow. This completes end-to-end validation for the tested FUID
and source dump.

AMS Lite placement is critical, especially for small round tag antennas. Each
slot's reader is a large circular copper loop inside the white spool backing
plate, near its outer perimeter. Mount the tag flat on the rotating inner spool
face, radially aligned with that loop, with the smallest practical plastic/air
gap. A tag placed near the center axle was not detected. Holding the tag
stationary against the reader produced inconsistent refresh results because a
normal scan expects the rotating tag to enter and leave the field. Moving the
same tag outward onto the reader-loop path made the tested clone recognizable.

The UID-test path is designed to change only UID/BCC, verify the
temporary manufacturer block, restore the original block, and verify it
byte-for-byte. It repeats the full 64-block preflight immediately before the
write. After any ambiguous temporary-write result it repeatedly probes both
UIDs, treats a fully verified original block as restored, and reissues the
restoration whenever the temporary UID can be authenticated. Power loss or RF
loss can still leave a test tag with the temporary UID, so use only a
replaceable sacrificial tag and keep it centered until final restoration is
reported.

The tested USB-UART adapter held the ESP32 in reset when its serial port was
closed. `platformio.ini` therefore opens the monitor with DTR and RTS inactive.
If the web UI disappears immediately when a monitor closes, leave the monitor
open or power the ESP32 through a charge-only/external supply that does not
connect USB reset-control signals. This is an adapter/auto-reset behavior, not
a PN5180 RF failure.

Supplying only the PN5180's 3.3 V rail can allow UID reads while causing
intermittent authentication or block access. If UID detection works but full
reads do not, verify both power rails before changing software.

## Optional PN532 compatibility

PN532 remains available for read-focused compatibility, but it is not the
primary target and does not provide the hardware-validated FUID workflow.
Set the module's jumpers or DIP switches to **SPI mode** first; positions vary
by board.

| PN532 | ESP32 DevKit |
|---|---:|
| 3.3V | 3.3V |
| GND | GND |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| SS / SSEL / CS | GPIO 5 |

Build it explicitly with:

```sh
pio run -e pn532 -t upload
```

Very small sticker or coin antennas may detune when pressed directly against
the PN532 board. If a known 13.56 MHz ISO14443A tag produces no UID, try a
non-metallic 5–10 mm spacer and rotate the tag slowly. PN532 cannot detect
125 kHz LF tags, UHF tags, or non-ISO14443A NFC protocols.

## Optional RC522 compatibility

The RC522 target is retained for read-focused compatibility:

```sh
pio run -e rc522 -t upload
```

## Firmware updates over Wi-Fi

Two password-protected OTA methods are available after the ESP32 joins the home
network. The default OTA password is `bambu-rfid`; change
`RFID_OTA_PASSWORD` in `include/config.h` before using the device on an
untrusted network.

### Web browser

1. Build the firmware without uploading it:

   ```sh
   pio run -e pn5180
   ```

2. Open `http://bambu-rfid.local/`.
3. In **Firmware update over Wi-Fi**, choose
   `.pio/build/pn5180/firmware.bin`.
4. Enter the OTA password and confirm the update.
5. Keep the ESP32 powered until it validates the image and restarts.

The endpoint rejects updates while an RFID operation is active and uses the
ESP32 Update API to validate the uploaded application image before rebooting.

### PlatformIO / ArduinoOTA

The primary `pn5180-ota` environment targets `bambu-rfid.local`:

```sh
pio run -e pn5180-ota -t upload
```

If mDNS is unavailable, replace `bambu-rfid.local` in `platformio.ini` with the
station IP shown in the web UI. Keep the `--auth` value in `platformio.ini`
synchronized with `RFID_OTA_PASSWORD` if the default is changed.
The optional `pn532-ota` environment is retained for PN532 compatibility.

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

## Reading a tag

1. Open the web UI and leave **Auto-scan** enabled.
2. Keep the spool face flat over the PN5180 and rotate it slowly as indicated.
3. Stop when the UI reports that a tag was detected.
4. Review the decoded tag and GitHub library information.
5. Download the complete 1 KiB dump if needed.
6. Remove the spool; the UI keeps the result clearly marked as **Last scan**.

Writing remains irreversible and is enabled only for the guarded, validated
PN5180 FUID workflow described above. There is no warranty that a marketplace
product matches its advertised chip generation.

## Credits

- [Bambu Research Group RFID Tag Guide](https://github.com/Bambu-Research-Group/RFID-Tag-Guide)
  for documenting the Bambu Lab RFID format, key derivation, and tag research.
- [Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library)
  and its contributors for collecting and organizing community tag dumps and
  filament metadata.
- [Elechouse PN5180 Library](https://github.com/wilson-elechouse/PN5180_ELECHOUSE)
  for bounded BUSY/reset timeouts, ISO14443A, and native MIFARE Classic
  authentication and block access in the primary PN5180 target.
- [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532) for the optional
  PN532 compatibility build.
- [MFRC522](https://github.com/miguelbalboa/rfid) for optional RC522 read-only
  compatibility.
- The Arduino, Espressif, and PlatformIO communities for the ESP32 framework,
  networking, OTA, and build tooling used by this project.
