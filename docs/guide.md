# The Camera SDK, in depth

The [README](../README.md) shows how to start. This page is the reference behind it: what each part of the SDK does, how the owner works, what an app pays for it and what can surprise. [`camera.h`](../components/camera/include/camera.h) and [`camera_sys.h`](../components/camera_sys/include/camera_sys.h) are the contract: every call's comment says what it does, what it refuses and, in the first, whether it WAITS.

## What it drives

The calls are listed by their task-layer names (`camera.h`); the same ones exist in `camera_sys.h` as `camera_sys_<name>()`, and [the two layers, call by call](#the-two-layers-call-by-call) gives each pair. A call that takes an answer is a *request* there (the asking Active Object first, the answer a `CAMERA_<NAME>_RSP_SIG`); one that only changes something is a *command*.

| Part | Calls | Notes |
|---|---|---|
| The camera | `camera_begin()`, `camera_is_present()`, `camera_wait_present()`, `camera_status()`, `camera_state_name()`, `camera_header_position()`, `camera_standby()`, `camera_end()`, `camera_error()` | Found by its fingerprint, upright or turned round, plugged in before or after. `camera_status()` says where it is (looking, no sensor, running...), which board (v1.0, v1.1), which way round, the frame rate and what it says about the 24 MHz clock, and whether a big photo is being made. `camera_header_position()` gives the J3 position that was set for one of the module's lines, for its board and its way round. `camera_error()` says, for the task that asks, why its last call failed. |
| Frames | `camera_frame_take()`, `camera_frame_give()` | The newest frame not taken yet: 800 x 600 RGB565 at 14.3 a second, held for you until you give it back or take the next one. The camera never writes over a frame you hold. `camera_sys_frame_take()` never blocks, so an Active Object can take frames. |
| A picture of any size | `camera_fit()`, `camera_fit_with_options()`, `camera_fit_point()`, `camera_pixel()` | The frame upright, into any size: cropped to fill (`CAMERA_COVER`), whole with bands (`CAMERA_CONTAIN`) or squashed (`CAMERA_STRETCH`). Optionally mirrored, zoomed up to 16x and panned, averaged where it is reduced, or with the bytes the other way round. `camera_fit_point()` turns a touch on such a picture into a point of the frame. On the P4, a smooth cover or stretch into a buffer aligned to 128 bytes is made by the PPA (800 x 600 to 640 x 480: 8 ms, against 180 ms by the CPU). |
| Photos | `camera_photo()`, `camera_photo_with_options()`, `camera_photo_free()`, `camera_photo_save()`, `camera_photo_save_with_options()`, `camera_photo_recover()` | A JPEG, upright, with an EXIF: 800 x 600 in some tens of milliseconds, 1616 x 1208 (2 MP) or 2592 x 1944 (5 MP) in a second or two. A crop, a smaller size, a mirror, a zoom, a focus pass first. In memory, or straight to a file, written whole or not at all. |
| The big photos' memory | `camera_big_photo_memory_reserve()`, `camera_big_photo_memory_release()` | 23.4 MB of PSRAM, kept so that the second photo finds room. |
| Focus | `camera_focus_mode()`, `camera_focus()`, `camera_focus_point()`, `camera_focus_at()`, `camera_focus_info()`, `camera_focus_firmware()`, `camera_focus_firmware_file()` | A contrast autofocus that runs on its own; one pass on demand or on a touched point; the lens by hand. The sensor's own microcontroller is an option an app can give its firmware to. |
| Video | `camera_video_start()`, `camera_video_stop()`, `camera_video_info()`, `camera_video_repair()` | QuickTime, MJPEG pictures and the Dot's microphone, to the card, in the background. A power cut loses at most the last second. |
| Settings | `camera_set()`, `camera_get()`, `camera_setting_info()`, `camera_settings_reset()`, `camera_sensor_info()`, `camera_register_read()` | 23 of them (below), kept across restarts. What the sensor's automatic exposure, gain and balance chose can be read back. Any register can be read. Turning the three corrections (lens, pixel, gamma) off at once may restart the camera. |
| Events | `camera_event_add_listener()`, `camera_event_remove_listener()`, `camera_event_wait()`; in `camera_sys.h` `camera_sys_subscribe()` | By name: the camera found (`CAMERA_EVENT_MODULE_FOUND`) or gone, a module on J3 that is not this camera, a focus pass ended, a movie closed, no memory for the frames, the settings back in the sensor or lost, a photo taken, the microphone refused for good. A task's listener runs on a task of the SDK's own, not the camera's, one event at a time (eight listeners, a queue of sixteen); an Active Object gets the same as `CAMERA_*_SIG` news. |
| Any register | `camera_lab_register_write()` | Writes any register, in a build made with `CONFIG_CAMERA_LAB`, and is refused in every other. |

### The two modes

The SDK moves between them on its own.

- **The preview**, always on. OV5640, 8-bit DVP, RGB565, 800 x 600, **14.3 frames a second**. That is the sensor's real rate from its 24 MHz oscillator (pixel clock 56 MHz, two clocks a pixel, 1896 x 1034 per frame); the format table is named "10fps" and the name is wrong about it. The `fps_x10` field of `camera_status()` is the rate measured from the frames' own timestamps: 143. The frames, the screen, the movies and the 800 x 600 photos come from here. Any other size you want on the screen is a `camera_fit()` of the frame.
- **The still**, for a photo of `CAMERA_RESOLUTION_2MP` or `CAMERA_RESOLUTION_5MP`. The preview goes out for about a second and a quarter: the sensor is given its whole array (2592 x 1944, five frames a second) at the preview's exposure and gain, held by hand, the first whole frame is kept, and the preview comes back with your settings, the lens where it was and the balance as it was. When the preview's light cannot be carried, fifteen frames go by while the sensor's own exposure settles at the longer line, some four seconds. 2 MP is that same frame reduced to 1616 x 1208. While it lasts no frame arrives, the frames you hold stay valid, the calls that wait time out, and the `big_photo` field of `camera_status()` is true.

The SDK writes the sensor's clocks, frame timing, window and pads itself, in those two modes and nowhere else. `camera_set()` offers only what does not change them, and exposure is capped at one frame so it cannot stretch the frame. A register past that lock can be written only in a `CONFIG_CAMERA_LAB` build.

### Focus

The lens sits on a motor that the sensor drives with a 10-bit number: 0 is far away, 1023 is close. The SDK finds the focus by looking at the picture. A sweep over the lens's range scores the sharpness of a window in the frame, a finer search follows around the best place, and the lens goes there. No firmware is loaded. It is on from `camera_begin()`: the first pass runs about a second after the first frames, and a new one starts whenever the picture changes for good. A pass is expected to take about a second and a half (`camera_focus_info().pass_ms` says how long it took).

- **Touch to focus.** `camera_fit_point()` turns a touch on a picture made by `camera_fit_with_options()` - its fill, mirror, zoom and pan - into a point of the upright frame, and `camera_focus_point()` focuses there. The window stays on that point until `CAMERA_FOCUS_CENTRE` puts it back.
- **While a movie records** the lens keeps its focus: the picture changing does not start a search, which would show in the movie as the blur pumping in and out and be heard as a dozen clicks. A pass the app asks for (`camera_focus()`, `camera_focus_point()`) still runs, and the lens glides there in small steps instead of one jump; the movie's sound is turned down (-12 dB) from some 50 ms before the move to 60 ms after it, since the Dot's microphone hears the lens's motor through the case. When the movie ends, the scene is watched again, and a change that lasted through it starts a search at once. With the sensor's own microcontroller driving the lens (`camera_focus_firmware()`), it does not follow the scene during a movie either, but a pass the app asks for moves the lens its own way: neither glided nor hushed.
- **Modes.** `CAMERA_FOCUS_AUTO` is the default. `CAMERA_FOCUS_OFF` leaves the lens at the sensor's own position and nothing runs: for an app that does not want the lens to move. `CAMERA_FOCUS_MANUAL` is `camera_focus_at()`.
- **Photos.** A photo with `.focus = true` runs a pass first, whatever the mode. A big photo that does not ask for one still waits for a pass that is already running, so that the lens is not taken out of the preview half-way.
- **A scene with no detail** (a wall, a covered lens) has no focus to find: the pass says so (`camera_focus()` answers `KODE_ERROR`) and the lens stays where it was.
- **The sensor's own microcontroller** can drive the lens instead (`camera_focus_firmware()`, `camera_focus_firmware_file()`). Its firmware is not in this repository and is under a licence of its own: [`components/camera_focus_firmware/NOTICE.md`](../components/camera_focus_firmware/NOTICE.md). Nothing above needs it.

### Photos

`camera_photo()` and `camera_photo_save()` take the newest frame, upright, and the P4's hardware encoder makes the JPEG: quality 1..100, with an EXIF that holds the date (when the Dot's clock has been set), the exposure, the ISO and the module, and the position if the app gives one. `camera_photo_with_options()` and `camera_photo_save_with_options()` take a `camera_photo_options_t`: the resolution, a smaller size or another shape (`width`, `height`, `fill`), a mirror, a zoom with its pan, the quality, a focus pass first.

- **Saving is safe, and in one go.** The photo is written whole to `<path>.part`, in one open, one write and one close on the card (not through `kode_sys_file_write()`, whose pieces of 32 KB are each opened and closed); only then is an older file of that name set aside as `<path>.bak`, the new one takes its name, and the old one goes. A power cut at any moment leaves one whole copy, under one of the three names, and the next save of that path - or `camera_photo_recover()` at start-up - puts it back.
- **The big photos need room.** 23.4 MB of PSRAM: two frames of 10.1 MB and a block of 3.1 MB for the JPEG. Taken for each photo they fragment the PSRAM and the second photo finds no room, so an app that takes them calls `camera_big_photo_memory_reserve()` right after `camera_begin()`, while the PSRAM is whole. Without room a photo answers `KODE_NO_RESOURCES` and the preview is not touched.
- **Not while recording.** A big photo is refused (`KODE_DENIED`) while a movie is being recorded: it would freeze the movie. A movie cannot start during one either.
- **Exposure by hand** is kept in time, not in lines: the still's line is longer than the preview's, so the exposure is converted to last as long.

### Video

`camera_video_start()` records a QuickTime movie (`.mov`) to the card in the background: MJPEG pictures and, if asked, 48 kHz mono sound from the Dot's microphone. It plays on a computer, a phone and in VLC.

- **The rate.** The sensor gives 14.3 distinct pictures a second and the movie runs at 15 slots a second, an exact divisor of the sound's clock. About one slot in twenty repeats the picture before it; `camera_video_info().repeated` counts them. `fps` takes another rate (1..30 and a divisor of 48000): a lower one skips frames and a higher one repeats them.
- **Size and look.** The same crop, zoom and mirror as a photo, up to 800 x 600; every setting applies.
- **Sound.** kode-sdk records in pieces of half a second, so there may be tiny gaps between them; the SDK fills them with silence and keeps the sound on its time (`silence_ms` counts what it put in). How far the sound is from the picture is found with a clap and corrected with `audio_delay_ms` (0 by default): `examples/video` says how.
- **The start does not wait for the card.** `camera_video_start()` asks the card for the folder (which mounts it) and answers: the movie begins at the call, and its file and journal are made by the recorder while the first frames and sound wait in memory (a megabyte of frames, two and a half seconds of sound). A card that will not make them ends the movie at once with `video_ended` and `KODE_IO_ERROR`, as a card that fails while it records.
- **The length moves with the picture.** `camera_video_info().duration_ms` counts the frames grabbed while it records; at the end it is what the file holds.
- **Safe.** A journal next to the file lets `camera_video_repair()` close a movie that a power cut left unfinished, at its last whole second. A file is closed at 3.9 GB (FAT's limit) and the movie goes on in `<name>_2.mov`.

### Settings

`camera_set(setting, value)` changes one, `camera_get()` reads it, `camera_setting_info()` gives its name, range and default for a menu or a slider, and `camera_settings_reset()` puts them all back. They are kept across `camera_end()` and the next `camera_begin()`, until `camera_settings_reset()`, and set again whenever the camera is brought back: after a restart, a standby, a big photo, or unplugged and plugged in again. With no camera yet they are kept and set when one comes.

| Setting | Range | Default | What it does |
|---|---|---|---|
| `CAMERA_TEST_PATTERN` | 0..1 | 0 | The sensor's colour bars instead of the picture. |
| `CAMERA_EXPOSURE_MANUAL` | 0..1 | 0 | The exposure is `CAMERA_EXPOSURE`'s. Until that is set it stays what the automatic exposure had chosen, and so for a big photo. |
| `CAMERA_GAIN_MANUAL` | 0..1 | 0 | The gain is `CAMERA_GAIN`'s; until that is set, what the automatic gain had chosen. |
| `CAMERA_WHITE_BALANCE` | `camera_white_balance_t` | `CAMERA_WHITE_BALANCE_NEUTRAL` | Auto, sunny, cloudy, office, home, manual, or neutral: the sensor's own, corrected until the bright greys are grey. |
| `CAMERA_EXPOSURE` | 0..16480 | 0 | Sixteenths of a line, at most one frame; used while `CAMERA_EXPOSURE_MANUAL` is on, and 0 is never written (it would be no light at all). Read it back for what the sensor is using, the automatic exposure's choice included. |
| `CAMERA_GAIN` | 0..1023 | 16 | 16 is 1x. |
| `CAMERA_WHITE_BALANCE_RED`, `CAMERA_WHITE_BALANCE_GREEN`, `CAMERA_WHITE_BALANCE_BLUE` | 0..4095 | 1024 | The balance's gains, with `CAMERA_WHITE_BALANCE_MANUAL`; 1024 is 1x. |
| `CAMERA_EXPOSURE_COMPENSATION` | -3..3 | 0 | Half stops of the auto exposure's target. |
| `CAMERA_BRIGHTNESS` | -3..3 | 0 | The picture lifted or lowered after the exposure. |
| `CAMERA_CONTRAST` | -3..3 | 0 | |
| `CAMERA_SATURATION` | -4..4 | 0 | A tenth more or less colour a step. |
| `CAMERA_VIVID` | 0..1 | 1 | Red as strong as blue: the module's table leaves red weaker. |
| `CAMERA_EFFECT` | `camera_effect_t` | none | Negative, grayscale, red, green, blue, sepia. |
| `CAMERA_SHARPNESS` | -3..3 | 0 | 0 is the sensor's automatic sharpening. |
| `CAMERA_DENOISE` | 0..8 | 0 | 0 automatic, then stronger and stronger. |
| `CAMERA_FLICKER` | `camera_flicker_t` | auto | 50 Hz or 60 Hz, so that lamps do not band. |
| `CAMERA_GAIN_CEILING` | 16..1023 | 120 | The most gain the automatic exposure uses (7.5x). Lower is less noise in the dark, and darker. |
| `CAMERA_LENS_CORRECTION` | 0..1 | 1 | The corners as bright as the middle. |
| `CAMERA_PIXEL_CORRECTION` | 0..1 | 1 | Dead and hot pixels hidden. |
| `CAMERA_GAMMA` | 0..1 | 1 | The tone curve. |
| `CAMERA_NIGHT` | 0..1 | 0 | The sensor's night mode. A `CONFIG_CAMERA_LAB` build only: it stretches frames, so the rate everything here rests on falls. |

Zoom, crop, mirror and fill are not sensor settings but software on the frame: `camera_fit_with_options()`, `camera_photo_options_t` and `camera_video_options_t` carry them, they cost the sensor nothing and they apply to the photo, the movie and the screen alike.

## The two layers, call by call

`rc` below is `kode_status_t`. "Command" is a `camera_sys_` call that does something and returns at once, with no answer to wait for. "Request" takes the asking Active Object first and answers with a signal. "Immediate" returns a value at once.

| Task layer (`camera.h`) | Active Object layer (`camera_sys.h`) |
|---|---|
| `bool camera_begin()` | `rc camera_sys_begin()` (immediate; cannot check that `kode_init()` ran) |
| `bool camera_end()` | `rc camera_sys_end()` (immediate; its end is `CAMERA_MODULE_LOST_SIG`) |
| `bool camera_is_present()` | `bool camera_sys_is_present()` |
| `bool camera_wait_present(ms)` | none: subscribe and wait for `CAMERA_MODULE_FOUND_SIG` |
| `bool camera_status(&s)` | `rc camera_sys_status_read(&s)` |
| `camera_state_name(s)` | `camera_sys_state_name(s)` |
| `camera_header_position(p)` | `camera_sys_header_position(p)` |
| `bool camera_standby(on)` | `rc camera_sys_standby(on)` (command) |
| `camera_event_add_listener(name, fn)`, `camera_event_remove_listener()`, `camera_event_wait(name, ms, &ev)` | `camera_sys_subscribe(me)`, `camera_sys_unsubscribe(me)`, then `CAMERA_*_SIG` news |
| `camera_error()` | the `why` of an answer; `camera_sys_status_text(rc)` |
| `bool camera_frame_take(&f, ms)` | `rc camera_sys_frame_take(&f)` (never waits) |
| `camera_frame_give(&f)` | `camera_sys_frame_give(&f)` |
| `camera_fit()`, `_with_options()`, `_point()` (`bool`), `camera_pixel()` (`camera_color_t`) | `camera_sys_fit()`, `_with_options()`, `_point()`, `camera_sys_pixel()` |
| `bool camera_photo(q, &p)`, `camera_photo_with_options(&shot, &p)` | `camera_sys_photo(me, &shot)`; answer `CAMERA_PHOTO_RSP_SIG` with the photo |
| `camera_photo_free(&p)` | `camera_sys_photo_free(&p)` |
| `bool camera_photo_save(path, q)`, `camera_photo_save_with_options(path, &shot)` | `camera_sys_photo_save(me, path, &shot)`; answer `CAMERA_PHOTO_SAVE_RSP_SIG` |
| `bool camera_photo_recover(path)` | `camera_sys_photo_recover(me, path)`; answer `CAMERA_PHOTO_RECOVER_RSP_SIG` |
| `camera_big_photo_memory_reserve()` / `_release()`, all `bool` | `camera_sys_big_photo_memory(me, keep)`; answer `CAMERA_MEMORY_RSP_SIG` |
| `bool camera_set(s, v)` | `rc camera_sys_set(s, v)` (command) |
| `bool camera_get(s, &v)` | `camera_sys_get(me, s)`; answer `CAMERA_GET_RSP_SIG` |
| `bool camera_setting_info()` | `rc camera_sys_setting_info()` (immediate) |
| `bool camera_settings_reset()` | `rc camera_sys_settings_reset()` (command) |
| `bool camera_sensor_info(&i)` | `camera_sys_sensor_info(me)`; answer `CAMERA_SENSOR_RSP_SIG` |
| `bool camera_register_read(r, &v)` | `camera_sys_register_read(me, r)`; answer `CAMERA_REGISTER_RSP_SIG` |
| `bool camera_focus_mode(m)` | `rc camera_sys_focus_mode(m)` (command) |
| `bool camera_focus(ms)` | `camera_sys_focus(me, ms)`; answer `CAMERA_FOCUS_RSP_SIG` (`focused`) |
| `bool camera_focus_point(x, y, ms)` | `camera_sys_focus_point(me, x, y, ms)`; answer `CAMERA_FOCUS_RSP_SIG` |
| `bool camera_focus_at(lens)` | `rc camera_sys_focus_at(lens)` (command) |
| `camera_focus_info(&i)` | `camera_sys_focus_info(&i)` |
| `camera_focus_firmware()`, `_file()`, all `bool` | `camera_sys_focus_firmware(me, fw, len)`, `camera_sys_focus_firmware_file(me, path)`; answer `CAMERA_FIRMWARE_RSP_SIG` |
| `bool camera_video_start(path, &o)` | `camera_sys_video_start(me, path, &o)`; answer `CAMERA_VIDEO_START_RSP_SIG` |
| `bool camera_video_stop()` | `camera_sys_video_stop(me)`; answer `CAMERA_VIDEO_STOP_RSP_SIG` |
| `camera_video_info(&i)` | `camera_sys_video_info(&i)` |
| `bool camera_video_repair(folder, &mended)` | `camera_sys_video_repair(me, folder)`; answer `CAMERA_VIDEO_REPAIR_RSP_SIG` (`value` is how many) |
| `bool camera_lab_register_write()` | `camera_sys_lab_register_write(me, reg, mask, val)`; answer `CAMERA_LAB_RSP_SIG` |

### The events

| Event | Task layer (name) | Active Object layer (signal) |
|---|---|---|
| module found | `CAMERA_EVENT_MODULE_FOUND` | `CAMERA_MODULE_FOUND_SIG` |
| module lost | `CAMERA_EVENT_MODULE_LOST` | `CAMERA_MODULE_LOST_SIG` |
| wrong module | `CAMERA_EVENT_WRONG_MODULE` | `CAMERA_WRONG_MODULE_SIG` |
| focus done | `CAMERA_EVENT_FOCUS_DONE` | `CAMERA_FOCUS_DONE_SIG` |
| video ended | `CAMERA_EVENT_VIDEO_ENDED` | `CAMERA_VIDEO_ENDED_SIG` |
| memory exhausted | `CAMERA_EVENT_MEMORY_EXHAUSTED` | `CAMERA_MEMORY_EXHAUSTED_SIG` |
| audio lost | `CAMERA_EVENT_AUDIO_LOST` | `CAMERA_AUDIO_LOST_SIG` |
| setting lost | `CAMERA_EVENT_SETTING_LOST` | `CAMERA_SETTING_LOST_SIG` |
| setting restored | `CAMERA_EVENT_SETTING_RESTORED` | `CAMERA_SETTING_RESTORED_SIG` |
| photo taken | `CAMERA_EVENT_PHOTO_TAKEN` | `CAMERA_PHOTO_TAKEN_SIG` |

Task listeners run on one task of their own ("camera_events"), never on the camera's: a slow one cannot hold the owner. Up to eight listeners and four waiters; the queue holds 16 events and `camera_status()` counts the ones it lost (`events_dropped`). An Active Object's news are counted too (`news_dropped`), and so is an answer that finds its queue completely full, or the pool dry (`answers_dropped`): lost and said in the log, never QP/C's assertion. Size the queue by the rule in `camera_sys.h` and none is lost.

## How it works

`camera_begin()` (or `camera_sys_begin()`) switches J3's supply on (`kode_sys_exp_power()`, which needs `CONFIG_KODEOS_RADIO=y`; the module's supply when an app starts is not promised) and starts one Active Object, the **owner**: `AO_Camera`, of kode-sdk's own kind (`kode_sys.h`). Once a second it lends the whole header and looks for a camera without driving a single line. The module's SCL and SDA have 2.2 k pull-ups and its PWDN a 10 k pull-down. The owner reads the first two against the P4's pull-downs and PWDN against its pull-up, both ways round, since J401 fits J3 turned by 180 degrees as well. Where PWDN sits tells v1.0 from v1.1. Three readings that agree, 20 ms apart, and the camera is taken. A camera that the last app left streaming drives its data lines, so a fingerprint that is found is first told to stop over SCCB and then read again.

Then the owner brings it up: PWDN low, the DVP controller and its four buffers, the sensor at 0x3C answering with 0x5640, the format, the image flipped in the sensor for the way the module sits, the stream. From then on it watches. If no frame arrives for 1.5 s, everything is taken down and looked for again (unplugged, turned round, a glitch). After 15 black frames in a row the sensor is configured again: at most three times with no lit frame between them, and never under a manual exposure, so a covered lens or a dark room is not fought. The `restarts` field of `camera_status()` counts both, and the app's settings are set again each time.

A frame task of the owner's reads every frame: the black check, the brightness, the sharpness of the focus window and the grey balance. The autofocus runs inside the owner: it asks for a step when a frame has been measured and writes the lens right after that frame, so that the next one is exposed with it. A big photo and a movie are the owner's too; a recorder task and a microphone task do the movie's work.

Both layers ask the same owner. A task's call in `camera.h` is a request with a sequence number and a semaphore: the task sleeps until the owner answers, and a late answer to a task that gave up is recognised by its number and thrown away. An Active Object's request in `camera_sys.h` carries the asker and a signal instead: the owner posts the answer to its queue. What takes long - a big photo, a focus pass, opening a movie - the owner hands to a `kode_worker` of its own, and the requests that come meanwhile wait in its defer queue. Taking a frame does not: it holds the newest whole frame's buffer and returns. The calls that wait are marked WAITS in `camera.h`; a call of that kind made from an Active Object's task is refused (`false`, a line in the log that says to use `camera_sys.h`). A listener may make any call of `camera.h` but `camera_event_wait()`. `camera_is_present()`, `camera_status()`, `camera_header_position()` and `camera_frame_take()` with a wait of 0 return at once, and are allowed anywhere.

### The module's clock and standby

- **The clock.** The SDK checks the module's 24 MHz clock in two ways. A sensor that does not answer on SCCB is `CAMERA_NO_SENSOR`: no clock is reaching it, or the sensor is dead. The frame rate, measured over 5 s, is `CAMERA_CLOCK_GOOD` within 25 % of 14.3; `CAMERA_CLOCK_SLOW` catches a wrong oscillator, since a 12 MHz part gives 7.
- **Standby and end.** `camera_end()` stops the stream, sets the sensor's outputs to high impedance, puts it in software power-down and lets PWDN go (R502 holds it low) as J3 is given back. `camera_standby()` and every orderly restart, `kode_sys_exit_to_os()` included (a shutdown handler), do the same writes and then raise PWDN. A crash skips the handler. The sensor has no reset line on the header and its supply stays on, so after a crash it keeps streaming until the next app that starts the camera stops it, which the owner does before anything else.
- **What software cannot stop.** The module's oscillator runs while the module is plugged in, whatever the app does.

## Gotchas

- **The bytes.** A frame is RGB565 as the P4 keeps a `uint16_t`: read it as `const uint16_t *`, and red is bits 15..11. That is what LVGL's `LV_COLOR_FORMAT_RGB565` and the JPEG encoder take. For the high byte first, ask `camera_fit_with_options()` for `.swap`.
- **Turned round.** The sensor can flip only one way in hardware, so a module plugged in upside down gives frames upside down: `f.turned` says so. `camera_fit()`, `camera_pixel()`, photos and movies turn them for you; if you read `f.pixels` yourself, you turn them.
- **One taker.** Take frames from one task. Each take gives back the frame taken before.
- **The lens moves on its own.** The autofocus sweeps the lens once at the start and whenever the picture changes. Not while a movie records (see Focus). An app that wants it still asks for `CAMERA_FOCUS_OFF` or `CAMERA_FOCUS_MANUAL`.
- **A big photo is a pause.** For about a second and a quarter there are no frames. Show something that says so; the `big_photo` field of `camera_status()` is true while it lasts.
- **Photos wait for the card.** `camera_photo_save()` writes the JPEG through the volume kode-sdk mounted and returns when the card has it: from a task. From an Active Object `camera_sys_photo_save()` returns at once and `CAMERA_PHOTO_SAVE_RSP_SIG` says when it is on the card. The folder must exist (`kode_file_create_folder()`, or `kode_sys_file_*` from an Active Object). A photo taken is never thrown away without a word: a save that fails says so, in the answer and in the log.
- **The 160 MHz clock.** ESP-IDF 6.1 counts the users of each PLL output in 16 bits, and its SD host takes a reference to the 160 MHz one on every card transaction and never gives it back. After some 32 000 transactions - ten photos and a gallery are enough - the count wraps, and the next driver that lets go of that clock (the codec, when a movie starts its sound) switches it off under the I2S, the SD host and the DVP: the app stops on the task watchdog. `camera_drv/clock_ref` wraps `esp_clk_tree_enable_src()` at the link and keeps ESP-IDF's count of that clock from passing 1024; `camera_drv_clock_ref_kept()` says how many references it holds past that. An app linked with this SDK carries it; nothing to call.
- **No flash.** The module has no light and no flash, so there is no call for one. An app that wants light drives its own.
- **The card and the C5.** In this board the C5 switches the microSD's supply, so an app that writes to the card sets `CONFIG_KODEOS_RADIO=y`, with the Wi-Fi and the network off (`KODEOS_CHANNEL=n`; BLE is left as kode-sdk sets it). Kode's Camera app, built this way on kodeOS 0.1.5, saves photos and movies to the card. When the C5's link comes up, a second or two after the start, the camera may restart once (`CAMERA_EVENT_MODULE_LOST`, then `CAMERA_EVENT_MODULE_FOUND`); a call made in that moment answers `false` or `KODE_DENIED`.

## Examples

One ESP-IDF project per part, each an app for kodeOS laid out as kode-sdk's examples are. Two are written for the task layer (`camera.h`) and four for the Active Object layer (`camera_sys.h`): the first group reads as a script that waits for the camera, the second as a state machine that never does. **Nothing here records, takes a photo or sends anything by itself**: each waits for a button, a touch or a console command, and `module` only reports.

| Example | What it shows |
|---|---|
| [`examples/module`](../examples/module) | Task layer. The camera found on J3 and what the owner knows of it (`camera_status()`): the board, which way round, the frame rate and the clock, where its lines arrive on J3. Noticed when it leaves, with listeners. Needs no card. |
| [`examples/console_actions`](../examples/console_actions) | The camera on kodeOS's console, for a terminal, BLE and kode-cli: fifteen functions (photos, focus, lens, standby, the looks, a movie) and ten variables. Active Object layer. |
| [`examples/photo`](../examples/photo) | Active Object layer. The top button saves an 800 x 600 photo to the microSD (`/sdcard/photo.jpg`), and a short press of the bottom button a 5 MP one in focus (`/sdcard/photo_5mp_N.jpg`), with the big photos' memory reserved first (`camera_big_photo_memory_reserve()`). Pull the module out and put it back, turned or not. |
| [`examples/viewer`](../examples/viewer) | Active Object layer. The camera full screen with LVGL through kode-sdk's `kscreen`: the pad zooms and mirrors, and a touch focuses where it lands, with a ring that shows the pass. |
| [`examples/settings`](../examples/settings) | Task layer. The top button starts a tour of the settings: lists each with its range, default and value, then saves to the card one photo of each look (exposure, brightness, contrast, saturation, balances, effects, zoom, mirror, a 2 MP photo with a look on it) and logs what the sensor chose for it. |
| [`examples/video`](../examples/video) | Active Object layer. The top button starts, and stops, thirty seconds of video with sound to the card, a report every second, and the clap test for the sound's offset. |

Run every one with the module on J3 - plugged in with the Dot off - and the Dot on a USB-C cable; all but `module` and `viewer` write to a microSD. Their `sdkconfig.defaults` set `CONFIG_KODEOS_RADIO=y` with Wi-Fi and the network off, and the five OV5640 lines: copy those, not kode-sdk's examples' `KODEOS_RADIO=n`.

## Options

| Kconfig | Default | What it does |
|---|---|---|
| `CONFIG_CAMERA_AO_PRIORITY` | 0 (`KODE_PRIO_USER_HIGH`) | The QP/C priority of `AO_Camera`. Two Active Objects at one level stop the board when the second starts: an app whose own AO needs the high level moves this one. |
| `CONFIG_CAMERA_LAB` | off | Any register of the sensor (`camera_lab_register_write()`) and `CAMERA_NIGHT`: for trying a register by hand. Never in an app that ships. |
| `CONFIG_KODEOS_RADIO` | `y` in every example | J3's supply and the microSD's are switched by the Dot's C5, over the radio link. Without it J3 is always powered (`CAMERA_SUPPLY_ALWAYS_ON`), `camera_end()` cannot switch the module off, and the card cannot be used. |
| `CONFIG_CAMERA_SIG_OFFSET` | 1200 | Where the camera's signals begin above `MAX_SIG` (`CAMERA_SIG_FIRST`); move it if an app's own signals collide. |
| `CONFIG_CAMERA_OV5640*` | the five lines in the README's "Use it in your project" | The OV5640's format table the SDK starts from; the build stops without the first, fourth and fifth. |

## The console

[`examples/console_actions`](../examples/console_actions) puts the camera on kodeOS's console as rows a person or kode-cli can type: `function name="cam_photo" arg="/sdcard/a.jpg"`, `variable name="cam_state"`. As in kode-sdk, the rows are the app's: one that wants them copies the example's tables, or the rows it needs. A function answers `>ok` when accepted and the outcome goes to the log and to `cam_last`: the example is an Active Object, so a photo or a focus pass is a request of `camera_sys.h` and its answer arrives later as a signal. The rows start with `cam_`, not `camera_`: the console takes names of 15 characters at most.

| Functions | Variables |
|---|---|
| `cam_photo`, `cam_big_photo`, `cam_focus`, `cam_focus_mode`, `cam_lens`, `cam_standby`, `cam_balance`, `cam_effect`, `cam_brightness`, `cam_contrast`, `cam_saturation`, `cam_exposure_ev`, `cam_video_start`, `cam_video_stop`, `cam_defaults` | `cam_state`, `cam_fps`, `cam_frames`, `cam_restarts`, `cam_rotated`, `cam_lens_now`, `cam_focus_now`, `cam_video`, `cam_big_now`, `cam_last` |

## What it costs an app

| Resource | What the camera takes |
|---|---|
| The header | All 14 positions of J3, lent under the name "camera"; the console's `exp` shows them. While the camera runs, no other module can be on J3. |
| The P4 | The DVP controller 0, I2C port 1 (the SCCB), and the JPEG encoder, which is one engine: photos and movies take turns. |
| PSRAM, always | Four frame buffers of 960 KB, 3.84 MB, and the encoder's input block of 960 KB once the first photo or movie needs it. |
| PSRAM, a 2 MP or 5 MP photo | 23.4 MB more while the photo is made, or for as long as `camera_big_photo_memory_reserve()` keeps it. |
| A movie | About 2.3 MB of heap: a megabyte for the frames that wait for the card, a ring of 512 KB, a JPEG block of 384 KB and some 360 KB for the sound. |
| The owner | `AO_Camera` at `KODE_PRIO_USER_HIGH` (`CONFIG_CAMERA_AO_PRIORITY` moves it): its stack in internal RAM, a queue of kode-sdk's length, a defer queue of four requests, one pool event per request, and a `kode_worker` (`camera_work`, 8 KB of stack in PSRAM). |
| Tasks | `camera_frame` (priority 2, 3 KB), and while a movie is recorded `camera_grabber` (4, 6 KB), `camera_recorder` (3, 6 KB) and `camera_microphone` (3, 4 KB). |
| Time | An 800 x 600 photo, some tens of milliseconds. A 2 MP or 5 MP photo, about a second and a quarter with the preview out, and 1.2 to 1.5 s more to write a 5 MP photo to a card on one line. |
| Not used | The screen, the radios, and the audio but for a movie with sound. |

[`tests/target/ram_budget.txt`](../tests/target/ram_budget.txt) holds what an app that uses the module starts with in internal RAM, and `tools/check_ram.sh` keeps it there.

## Layout

The tree is kode-sdk's: one component per layer, each part in a folder of its own with its header in `include/` and its source in `src/`.

| Path | What it holds |
|---|---|
| `components/camera` | The task layer (`kode` in kode-sdk): `include/camera.h`, what a task calls, and `src/camera.c`. |
| `components/camera_sys` | The Active Object layer (`kode_sys` in kode-sdk): `include/camera_sys.h`, what an Active Object calls, and `src/camera_sys.c`. |
| `components/camera_types` | The types both layers share (`camera_types.h`): settings, options, status, frames. |
| `components/camera_aos` | The owner (`kaos` in kode-sdk): what the layers include in `core/include/camera_aos.h`, and in `camera/src` the Active Object `AO_Camera` (`camera_ao.c`), what its signals do (`camera_aos.c`), the seam between them (`camera_ao.h`), the owner's rules in plain C (`camera_plan.c`: the watch, the rate, the sizes) and the movie recorder (`camera_recorder.c`, with its task and the microphone's). |
| `components/camera_drv` | The drivers (`kdrv` in kode-sdk), a folder each: `pins`, `ov5640` (SCCB, `esp_cam_sensor`), `dvp` and its buffers, `jpeg`, `ppa` (the P4's scaler, for the viewfinder), `still` (the 5 MP photo). `clock_ref` (the 160 MHz clock's count, see above). In plain C: `fingerprint`, `pixels`, `registers` (and the lock on the clocks), `buffers`, `contrast_focus`, `autofocus` (the sensor's own microcontroller), `white_balance`, `exif` and `movie` (the muxer). |
| `components/camera_bsp` | The module described (`kbsp` in kode-sdk), in `board/include/camera_bsp.h`: J3 positions for both boards, the fingerprint, the timings of the preview and the still. Only describes. |
| `components/camera_focus_firmware` | An addition of this module: the place for the sensor's autofocus firmware, which is not in the repository (its `NOTICE.md` says why). |
| `examples/` | One project per part. The two every module SDK has: `module` (task layer; presence, status and events: run it first) and `console_actions` (Active Object layer). Then this module's own: `photo`, `viewer` and `video` (Active Object layer) and `settings` (task layer). |
| `tests/host` | The rules that are plain C, with Unity and nothing else, built for ESP-IDF's `linux` target: `camera_plan` and the nine plain-C drivers. |
| `tests/host_drv` | The drivers that speak to ESP-IDF - `pins`, the sensor (`ov5640`), `dvp`, `jpeg` and `still` - over fakes of the peripherals they use and a sensor of 64 K registers (`main/fakes/`); the sensor's driver is built a second time as a `CONFIG_CAMERA_LAB` build. |
| `tests/host_owner` | The real owner (`camera_aos.c`) and the recorder on FreeRTOS for the `linux` target, over a simulated module - its registers, a lens with three focus zones, exposure, balance, DVP and JPEG - behind a stand-in for `AO_Camera`: found, gone, turned or the wrong one, the events, photos big and small, focus, settings, the movie and its repair, sleep and home. |
| `tests/host_ao` | `AO_Camera` itself on kode-sdk's real QP/C, at the pinned release: its queue's and pool's margins, the one-shot tick, the worker and the defer queue, `LOWPOWER_ROUND_SIG` and `HOME_SIG`. |
| `tests/host_standard` | The task layer, `camera.c`, over an owner that writes down what it is asked (`tests/fake_owner/fake_camera_aos.c`): what it refuses, how it asks the rest, the listeners and the waits. |
| `tests/host_sys` | The Active Object layer, `camera_sys.c`, over the same faked owner and a real QP/C: the doors, the queue rule, the answers and the news. |
| `tests/host_console` | `examples/console_actions` over a `camera_sys.h` that writes down what it is asked, with kodeOS's console registry and its rules: every row taken, every function making the call its name says, and every variable a word the console sends. |
| `tests/target` | The SDK built for the esp32p4 against the pinned `kode-sdk`, every call of both layers named in `ALL[]` and linked, and its RAM budget (`ram_budget.txt`). |
| `tests/sanitize.cmake` | ASan and UBSan for the host suites, under `tools/host_tests.sh --sanitize`. |
| `docs/` | This page. |
| `tools/` | `host_tests.sh`, `check_layers.sh` (fourteen rules), `check_misra.sh` (MISRA C with cppcheck against a baseline file you pass; the CI runs only its tests), `check_ram.sh`, `check_example_pins.sh`, `set_version.sh` (moves every manifest, the code and the README to one version for a release), and the checks' own tests. |
