# Spool Scanner (Flipper Zero app)

Tap Bambu Lab filament spools to the back of the Flipper. Each roll is decoded (material, product name,
color, weight, diameter, length, production date) and saved to
`/ext/apps_data/spool_scanner/spools.csv` on the SD card. Scanning the same roll twice does not count it twice.

Sync: in Spool Inventory click **Sync from Flipper (USB)** (Chrome/Edge; close qFlipper first), or copy
`spools.csv` off the SD card and use **Import spools.csv**. See the [main README](../README.md).

## Build / install

Needs [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) (Python 3.8+):

```bash
pip install --upgrade ufbt
ufbt update          # SDK for the official release firmware (default)
ufbt                 # build -> dist/spool_scanner.fap
ufbt launch          # install to /ext/apps/NFC/ and open it (Flipper on USB, qFlipper closed)
```

The SDK must match the firmware on your Flipper, or the app refuses to start with an **API mismatch**.
For other firmware, point ufbt at its SDK index first, for example:

```bash
ufbt update --index-url=https://up.unleashedflip.com/directory.json              # Unleashed
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json       # Momentum
```

Tip: to keep several SDKs side by side, set `UFBT_HOME` to a different folder per firmware.
After you update the Flipper's firmware, run `ufbt update` again and rebuild.

Tested on hardware with Unleashed (API 88.9). Builds cleanly against the official 1.5.1 SDK (API 88.2).

## How it reads the tag

Bambu tags are MIFARE Classic 1K. Sector keys are derived from the tag UID with HKDF-SHA256 (salt and info
from the Bambu Research Group [RFID-Tag-Guide](https://github.com/Bambu-Research-Group/RFID-Tag-Guide)).
The app reads blocks 1, 2, 4, 5, 9, 12, 14, 16 with key A. Block 9 (tray UID) identifies the roll.
The tag stores the roll's *starting* weight, not what is left.

Files: `spool_scanner.c` (UI, NFC, SD card), `bambu.c` (key derivation + decoding), `sha256.c`
(SHA-256/HMAC/HKDF; this keeps the app self-contained).
The key derivation was checked on a PC against PyCryptodome's HKDF (the reference implementation) and
RFC 5869 test case 1.

The app only reads tags. It does not write, clone or modify them.
