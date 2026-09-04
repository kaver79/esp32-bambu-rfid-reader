# Easy Install and FUID Tag Programming Guide

This guide builds the hardware, installs the PN5180 firmware, and creates an
exact clone of an existing signed Bambu Lab filament tag dump. It is written
for a classic ESP32 DevKit/WROOM board and the PN5180 module tested by this
project.

## Read this before writing

The firmware cannot generate a Bambu Lab signature. Material, color,
temperature, production, and other filament fields are signed. Editing any of
those fields invalidates the signature used by stock Bambu firmware.

The supported programming operation is therefore an **exact copy** of a
complete, signed 1,024-byte library dump onto one explicitly supported FUID.
The source dump determines every property shown by the printer.

Writing an FUID is irreversible:

- use a spare tag for the first test;
- keep only one RFID tag near the PN5180;
- do not move the tag or interrupt power during a write;
- do not continue unless the blank tag reports factory UID `AA55C396`;
- never substitute a CUID, Gen1, UFUID, NTAG, or ordinary fixed-UID card.

## What you need

### Hardware

- Classic ESP32 DevKit/WROOM development board (`esp32dev`).
- PN5180 ISO14443A reader module with separate `5V`, `3.3V`/`PVDD`, `GND`,
  `MOSI`, `MISO`, `SCK`, `NSS`, `BUSY`, and `RST` connections.
- Short Dupont wires, preferably 10 cm or shorter.
- A known genuine Bambu filament tag for the first read test.
- At least two unused FUID tags advertised with factory UID `AA55C396`.
  Keep one unused as a spare.
- A data-capable USB cable and stable USB power.
- A phone or computer on the same Wi-Fi network as the ESP32.

### Software

- Git.
- Python 3.
- [PlatformIO Core](https://platformio.org/install/cli) or VS Code with the
  PlatformIO extension.
- A modern web browser.

## Step 1: Wire the PN5180

Disconnect USB power before changing wiring.

| PN5180 | ESP32 DevKit |
|---|---:|
| 5V | 5V / VIN |
| 3.3V / PVDD | 3V3 |
| GND | GND |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |
| NSS / CS | GPIO 5 |
| BUSY | GPIO 4 |
| RST | GPIO 2 |

Important power details:

- Both PN5180 supply pins are required on the tested carrier.
- `5V` powers the RF/output stage; `3.3V`/`PVDD` is the logic
  supply/reference.
- SPI, BUSY, NSS, and RST are 3.3 V signals.
- Never connect 5 V to `PVDD` or an ESP32 GPIO.
- GPIO 2 is an ESP32 boot strap pin. If the ESP32 stops booting normally,
  disconnect PN5180 RST while resetting and verify that the module is not
  driving that pin incorrectly.

Supplying only 3.3 V may produce misleading partial behavior: UID detection
can work while authentication and full block reads fail.

## Step 2: Download the project

```sh
git clone https://github.com/kaver79/esp32-bambu-rfid-reader.git
cd esp32-bambu-rfid-reader
```

PN5180 is already the default PlatformIO environment. No reader-selection edit
is needed.

## Step 3: Build and install over USB

Connect the ESP32 with a data-capable USB cable, then run:

```sh
pio run -e pn5180
pio run -e pn5180 -t upload
pio device monitor -b 115200
```

The first build downloads the Arduino dependencies, including the maintained
PN5180 library fork. A successful build ends with `pn5180 SUCCESS`.

If upload auto-reset fails:

1. Run `pio device list` and identify the ESP32 serial port.
2. Hold the ESP32 **BOOT** button.
3. Tap **EN/RESET**.
4. Start the upload and release **BOOT** when connection begins.
5. Keep upload speed at 115200 baud.

The [release page](https://github.com/kaver79/esp32-bambu-rfid-reader/releases)
also provides a PN5180 `firmware.bin`. That application image is intended for
updating an ESP32 that already runs this project through the web updater or
ArduinoOTA. For a blank ESP32, use the PlatformIO USB procedure above so the
bootloader, partition table, and application are installed with the correct
layout.

## Step 4: Connect the ESP32 to Wi-Fi

On every boot the device starts a recoverable setup access point.

1. Connect a phone or computer to `Bambu-RFID-XXXXXX`.
2. Enter the default password `bambu-rfid`.
3. Open `http://192.168.4.1/`.
4. Expand **Network setup**, enter the home Wi-Fi details, and save them.
5. Reconnect the phone/computer to the home network.
6. Open `http://bambu-rfid.local/` or use the station IP printed in the serial
   monitor.

Saved station credentials take priority over compile-time defaults. The web UI
does not return the saved Wi-Fi password.

Before using the device on an untrusted network, change
`RFID_OTA_PASSWORD` in `include/config.h` and keep the `pn5180-ota`
authentication value in `platformio.ini` synchronized.

## Step 5: Prove the reader with a genuine tag

Do not begin with a write.

1. Open the web UI and confirm the header says `ESP32 + PN5180` and the status
   says the reader is ready.
2. Leave **Auto-scan** enabled or press **Scan now**.
3. Keep a genuine Bambu tag parallel to the PN5180 antenna.
4. Rotate or move it slowly until the scanning indicator becomes solid and the
   UI says the tag is detected.
5. Confirm that a complete decoded tag appears with UID, material, variant,
   color, weight, diameter, and production information.
6. Remove the tag. The UI must change from **Tag present** to **Last scan**.

A successful Bambu decode proves much more than UID detection: the reader has
authenticated and obtained the complete tag data. If only UID detection works,
recheck both PN5180 power rails and try a non-metallic 5–10 mm antenna gap.

## Step 6: Select an exact signed dump

The ESP32 must have internet access for this step.

1. In **Choose an exact signed library dump**, open **Library root**.
2. Browse the hierarchy:
   `Material category / Product / Color / Tag UID / Tag files`.
3. Select only the desired file ending in `-dump.bin`.
4. Wait for the ESP32 to download and validate it.
5. Verify the displayed product, official color, dump name, and source UID.

The write button is not safely usable until the ESP32 has accepted an exact
1,024-byte dump with a valid manufacturer UID/BCC, non-empty filament data,
and sector keys consistent with Bambu key derivation.

The hardware-validated example used:

```text
PLA / PLA Basic / Black / 02034E6D / hf-mf-02034E6D-dump.bin
```

Choosing that file creates an exact PLA Basic Black clone; it does not convert
an arbitrary record into a newly signed black PLA record.

## Step 7: Inspect the unused FUID

Remove every other tag from the PN5180 field, then present one unused FUID.

The first fast inspection checks only block 0. It should show:

- UID: `AA55C396`.
- Factory UID: `Matches AA55C396`.
- Detection: `Possible unused FUID — validation required`.
- Bambu write eligibility: `Full validation required`.
- The badge says **Tag present**, not **Last scan**.

Now select **Validate FUID (read-only)** and keep the tag still. After the
complete preflight succeeds, the inspection must show `16 / 16`, **Unused FUID
candidate**, and **Eligible for exact FUID cloning**. A blank FUID is not yet a
Bambu-compatible tag; compatibility begins only after an exact signed dump is
written and the complete readback succeeds.

Stop if any value differs. A UID that merely looks writable is not enough. The
separately displayed **Validate CUID (read-only)** and `TEST UID` controls are
an engineering test for non-FUID CUID tags; they do not make a CUID suitable
for a Bambu clone.

## Step 8: Program the FUID

This step permanently consumes the FUID.

1. Confirm the correct signed dump is still shown as loaded.
2. Confirm the currently present tag still shows factory UID `AA55C396` and
   `16 / 16` sectors.
3. Center the tag over the PN5180 and keep it motionless.
4. Type `WRITE` exactly, using uppercase letters.
5. Press **Write exact clone**.
6. Read the final browser warning and confirm only if the source and target are
   correct.
7. Do not touch the tag, wires, ESP32, or USB cable while progress is active.

The firmware writes ordinary data blocks first, then each sector trailer. It
verifies every sector under the new source key before continuing. Manufacturer
block 0—and therefore the one-time UID—is written last.

Accept success only when the UI reports:

```text
Write and full 1 KiB verification completed
```

The final detected UID must equal the source dump UID, and a new read must
decode the expected material and color. Any other message is not success.

## Step 9: Independent readback

1. Remove the programmed tag until the UI shows **Last scan**.
2. Present it again as a new tag.
3. Wait for a complete decode using the new source UID.
4. Compare material, variant, color, diameter, and weight with the selected
   library record.
5. Download the read dump if an additional archival copy is wanted.

This independent scan is separate from the write operation and confirms that
the completed tag can be selected and authenticated after RF state has reset.

## Step 10: Install and test on AMS Lite

AMS Lite placement is critical for small round tag antennas.

1. Mount the programmed tag flat on the **rotating inner face** of the spool.
2. Align it radially with the large circular RFID coil inside the white AMS
   Lite backing plate. The coil is near the backing plate's outer perimeter,
   not around the center axle.
3. Use thin tape or a thin holder and minimize the gap to the backing plate.
4. Do not tape the tag permanently to the stationary AMS body; it must enter
   and leave the RF field as the spool rotates.
5. Insert the filament and request an RFID refresh from Bambu Studio.
6. Confirm the AMS slot displays the expected material and color.
7. Perform a normal unload and reload, then confirm recognition again.

The tested small yellow FUID was not detected when mounted near the center hub.
Moving the same tag outward onto the AMS Lite reader-loop path made the stock
A1 Combo recognize it as PLA Basic Black, including after unload/reload.

## Interrupted-write recovery

Do not assume a failed message means nothing was written. Sector keys may have
changed even while the factory UID remains visible.

If an attempt stops and the tag still reports `AA55C396`:

1. Keep the tag centered and power stable.
2. Keep the **same exact source dump** selected.
3. Scan the tag again and record the complete UI error.
4. Retry only with that same dump and tag.

The PN5180 path checks each sector independently. A sector must either match
the selected source under its new key or remain readable under the factory key.
Anything else aborts recovery. Before block 0 is retried, all data outside the
manufacturer block must exactly match the selected dump.

If the UID has already changed to the source UID but final verification failed,
do not run another factory-FUID write. Remove and rescan the tag, save the exact
error, and diagnose the readback mismatch first.

## Troubleshooting checklist

### Reader unavailable

- Verify common ground.
- Verify PN5180 `5V` and `3.3V`/`PVDD` are both powered correctly.
- Verify MOSI 23, MISO 19, SCK 18, NSS 5, BUSY 4, and RST 2.
- Keep wires short and firmly seated.
- Check serial startup diagnostics at 115200 baud.

### UID appears, but authentication or full reads fail

- Recheck the PN5180 5 V RF supply.
- Try a 5–10 mm non-metallic spacer between small tags and the board antenna.
- Keep one tag in the field and remove nearby cards, key fobs, or spools.
- Use a stable USB supply and avoid loose breadboard connections.

### Library does not load

- Confirm the ESP32 station connection and IP in **Network**.
- Confirm general internet and DNS access.
- Retry later if GitHub anonymous API rate limits are reported.
- Do not paste arbitrary remote URLs; the firmware intentionally accepts only
  bounded paths from the configured public library.

### AMS Lite does not recognize a verified clone

- Confirm the web UI independently rereads the completed tag under its new UID.
- Move the tag outward to align with the AMS Lite circular reader loop.
- Keep it on the rotating inner spool face, parallel to the white backing
  plate.
- Remove thick adapters or spacers between the tag and reader.
- Unload filament before forcing another RFID refresh.

## Updating an installed device

Download the PN5180 application image from the latest GitHub release. In the
web UI:

1. Expand **Firmware update**.
2. Select the PN5180 `.bin` file.
3. Enter the OTA password.
4. Press **Install & restart** and approve the final warning.
5. Keep power connected until the ESP32 restarts and the reader returns ready.

Alternatively, when mDNS is available:

```sh
pio run -e pn5180-ota -t upload
```

Never claim an update succeeded until the upload completes and the device
returns with a healthy PN5180 reader status.
