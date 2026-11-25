# The autofocus firmware: read before you put it here

The OV5640 focuses its lens with a microcontroller inside the sensor. That microcontroller has no flash, so its firmware is written over the camera's bus after every power-up. The firmware is 4077 bytes (CRC-32 `0x0FF1228E`). `camera_drv_autofocus_firmware_ok()` refuses any other.

**This repository does not carry it.** The firmware is a third party's binary, and its licence is not the SDK's:

| Where it comes from | What it says |
|---|---|
| Adafruit's `CircuitPython_OV5640`, `ov5640_autofocus.bin` | `SPDX-License-Identifier: GPL-2.0-only`, copyright «2023 Unknown» |

The SDK is GPL-3.0-or-later. **GPL-2.0-only does not combine with GPL-3.0 in one work.** So an app's binary that embeds the firmware combines the two licences, and whoever distributes that binary must make sure they allow it. There are three ways:

1. **No firmware.** The SDK's own contrast autofocus moves the lens without it (the default). Nothing here is needed.
2. **The firmware embedded in the app** (this component). Put `ov5640_autofocus.bin` next to this file, add `kodediy/camera_focus_firmware` to the app's `idf_component.yml`, and call `camera_focus_firmware(camera_focus_firmware_data(), camera_focus_firmware_size())`. The app's binary then carries a GPL-2.0-only blob. **Check the licences before you distribute it.** A `*.bin` here is in `.gitignore` so that it is never committed by accident.
3. **The firmware on the card.** `camera_focus_firmware_file("/sdcard/ov5640_af.bin")` reads it at run time. The binary does not carry it, and whoever puts the file on the card brings it.

The code that loads and drives the firmware is the SDK's own: `camera_drv_autofocus.c`, GPL-3.0-or-later. Its order of operations follows Adafruit's MIT-licensed driver.
