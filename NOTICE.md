# Licences of kode-camera-sdk

This file says which licence applies to each part of this repository. Read it before you distribute the SDK, or a firmware that you built with it.

| Part | Owner | Licence |
|---|---|---|
| everything that this file does not name below | Kode | GPL-3.0-or-later, with the additional permission below |
| the OV5640's autofocus firmware, which this repository does **not** carry (`components/camera_focus_firmware/`) | its authors | GPL-2.0-only: [`components/camera_focus_firmware/NOTICE.md`](components/camera_focus_firmware/NOTICE.md) |

The SDK links [`kode-sdk`](https://github.com/kodediy/kode-sdk), which is GPL-3.0-or-later, so a firmware built with both is a covered work of that licence. `kode-sdk`'s own `NOTICE.md` says what applies to its parts.

## The code of Kode

Copyright (C) 2026 Kode.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The file `LICENSE` holds the full text of the GNU General Public License, version 3.

### Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the RF Abstraction Layer (RFAL) and the ST25R3916 driver of STMicroelectronics (or a modified version of those libraries), containing parts covered by the terms of the STMicroelectronics SLA0051 licence, the licensors of this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include the source code for the parts of RFAL and of the ST25R3916 driver used as well as that of the covered work.

kode-sdk carries RFAL and gives the same permission for its own code, so an app built with both SDKs can be distributed. If you change the code of Kode, you can remove this permission from your version. Section 7 of the GNU General Public License allows this.

### Trademarks, under GNU GPL version 3 section 7(e)

This licence gives no right to use the names Kode, Kode Dot, kodeOS and Kody, or the character Kody, as trademarks. `TRADEMARKS.md` in the repository `kodediy/.github` says how you can use them.

## Libraries fetched at build time

The ESP-IDF component manager fetches these when a firmware is built. Nothing from a third party is copied into this repository.

| Library | Owner | Licence | Why |
|---|---|---|---|
| `kode-sdk` | Kode | GPL-3.0-or-later (RFAL: SLA0051 of STMicroelectronics) | The Dot: J3, files, sound, logging. |
| `espressif/esp_cam_sensor` 2.6.0 | Espressif Systems | Apache-2.0 | The OV5640's driver and its format tables. |
| `espressif/esp_sccb_intf` 0.0.9 | Espressif Systems | Apache-2.0 | The SCCB bus the driver talks over. |
| ESP-IDF | Espressif Systems | Apache-2.0 | The DVP, I2C, JPEG, PPA and GPIO drivers. |

Apache-2.0 is compatible with GPL-3.0: a firmware that links them is distributed under GPL-3.0-or-later, with the Apache-2.0 notices of those libraries kept. Each one has its own licence in its folder under `managed_components/`.

## Where parts of the code come from

Everything in this repository is Kode's code, under the licence above. Part of it is built on the work of others, and this section says which, so that their notices are kept. No file of a third party is copied into this repository: what comes from a third party is register values and tables (facts about a chip, written here in this code's own layout) and the order of operations of drivers, read as documentation. The comments in the code say the same, beside the code.

### Third parties', under permissive licences

| What | Here | From | Licence |
|---|---|---|---|
| The whole-array window, the frame of 2844 x 1968, the registers 0x3618, 0x3612, 0x3709, 0x370C and 0x4001 of the full mode | `camera_drv_registers.c`, the still list, marked `[E]` | Espressif's `esp_cam_sensor` 2.6.0: `ov5640_mipi_2lane_24Minput_1280x720_rgb565_le_14fps.h` and the DVP 800 x 600 table | Apache-2.0 |
| The white balance presets, the brightness steps, the sharpness and denoise levels, the pixel corrections' bits, and the table of 0x4514 (the black level of the unbinned sensor) | `camera_drv_registers.c` | Espressif's `esp32-camera`, `sensors/ov5640.c` | Apache-2.0 |
| The contrast steps and the special effects' tables | `camera_drv_registers.c` | Adafruit's `CircuitPython_OV5640` | MIT |
| The order in which the sensor's own focus microcontroller is loaded and started | `camera_drv_autofocus.c`, `camera_drv_autofocus.h` | Adafruit's `CircuitPython_OV5640` | MIT |

Apache-2.0 and MIT ask that the notice of the work stays with what is taken from it. The notices are the comments beside each table and this section; the copyright lines of those projects are theirs (Espressif Systems, Adafruit Industries).

### The autofocus firmware

The OV5640's autofocus firmware (4077 bytes) is GPL-2.0-only and is not in this repository: `components/camera_focus_firmware/NOTICE.md` says what that means. The SDK's own autofocus, a contrast search, does not use it; an app that wants the sensor's microcontroller gives the firmware itself.
