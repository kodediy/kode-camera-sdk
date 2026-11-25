#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The layers, checked rather than described.
#
#   camera       the task layer: what a task calls, and may wait for   (camera.h)
#   camera_sys   the Active Object layer: what an AO asks, never waiting  (camera_sys.h)
#   camera_aos   the owner: one task that holds the module, decides when,
#                and serves both layers above it
#   camera_drv   the drivers: a part or a peripheral, nothing about who or when
#   camera_bsp   the module described: positions, timings, the preview and still modes
#
# Underneath, kode-sdk, reached through its facade and nothing deeper. The two
# upper layers do not know each other: both reach the owner through camera_aos.h.
# The rule not here - one owner for the module - is the owner's to keep: J3 is
# lent once, under CAMERA_BSP_LOAN_TAG, and a second loan answers KODE_EXISTS.
#
#   usage: tools/check_layers.sh
#   exit:  0 all clear, 1 something crossed a layer
#
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
report() {   # report <rule> <explanation> <matches>
    if [ -n "$3" ]; then
        printf '\n\033[31mFAIL\033[0m  %s\n      %s\n\n%s\n' "$1" "$2" "$3"
        fail=1
    else
        printf '\033[32mok\033[0m    %s\n' "$1"
    fi
}

# Build output and fetched components are skipped, and so are lines that look
# like prose: a comment that names esp_timer_get_time() is discussing it.
scan() { grep -rn --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
             "$1" components \
             2>/dev/null \
           | grep -v '/build/' \
           | grep -v 'managed_components/' \
           | grep -vE ':[0-9]+: *(\*|//|/\*)' \
           | { [ $# -gt 1 ] && grep -vE "$2" || cat; }; }

# ---- 1. ESP-IDF stops at camera_drv ----------------------------------------
# Above the drivers this is C that speaks kode_status_t. soc/ and hal/ are on
# the list because the register-level headers are not named esp_*.
# ov5640.h and sdkconfig.h are the camera driver's and the build's: the same
# rule, under names that do not start with esp_. The Active Object reads its
# priority from sdkconfig.h (CONFIG_CAMERA_AO_PRIORITY), and
# camera_sys.h from the same file the base of its signals (CONFIG_CAMERA_SIG_OFFSET):
# a header an AO includes carries its signals, as kode_sys.h does.
report "1. ESP-IDF stops at camera_drv" \
  "esp_*, driver/, soc/, hal/, ov5640 or sdkconfig above the drivers. Put it behind a driver." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\|ov5640\|sdkconfig\)' 'components/camera_drv/' \
     | grep -v '^components/camera_aos/camera/src/camera_ao\.c:[0-9]*:#include "sdkconfig\.h"' \
     | grep -v '^components/camera_sys/include/camera_sys\.h:[0-9]*:#include "sdkconfig\.h"')"

# ---- 2. FreeRTOS stops at the owner, the drivers and the task layer ---------
# The owner is a task, and its drivers share state with an interrupt under a
# spinlock. The task layer (camera.c) has a task of its own, the one that runs
# the app's callbacks, with its queue and its locks. The Active Object layer
# has none of that: its only FreeRTOS is the critical section around its own
# bookkeeping (FreeRTOS.h and task.h), and it never waits (rule 14).
report "2. FreeRTOS stops at camera_aos, camera_drv and camera" \
  "freertos/ outside those three. camera_sys.c may include FreeRTOS.h and task.h for a critical section, nothing else." \
  "$({ scan '#include "freertos/' 'components/(camera_aos|camera_drv|camera)/' \
         | grep -vE '^components/camera_sys/src/camera_sys\.c:[0-9]+:#include "freertos/(FreeRTOS|task)\.h"'; })"

# ---- 3. The module is described in one place ------------------------------
# A position, a timing or a mode is a fact about the Camera module. Two copies
# of a hardware fact drift apart, and the module is driven by the wrong one.
report "3. The module is described in one place" \
  "Module facts are defined only in camera_bsp: camera_bsp.h, and the name used everywhere else." \
  "$(scan '^#define CAMERA_BSP_' 'components/camera_bsp/')"

# ---- 4. The rules are plain C ----------------------------------------------
# Nine drivers - fingerprint (which camera), pixels (which bytes), registers
# (which registers: the lock on the clocks and the frame timing), buffers
# (which buffer), contrast_focus (the contrast autofocus), autofocus (the
# sensor's own focus microcontroller and its follow policy), white_balance
# (the neutral white balance), exif (a photo's EXIF), movie (the video file) -
# and camera_plan (every rule of the supervision) are run as they are by
# tests/host. An ESP-IDF, FreeRTOS or kode.h include there would take them
# off the host.
report "4. The rules are plain C" \
  "camera_plan and the nine plain-C drivers include neither ESP-IDF, FreeRTOS nor kode.h: they compute from what they are given." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\|freertos/\|kode\.h\|kode_log\.h\|ov5640\|sdkconfig\)' \
     | grep -E 'components/camera_drv/(fingerprint|pixels|registers|buffers|contrast_focus|autofocus|white_balance|exif|movie)/(include|src)/camera_drv_[a-z_]+\.[ch]:|components/camera_aos/camera/src/camera_plan\.[ch]:')"

# ---- 5. kode-sdk through its facade ---------------------------------------
# kode.h, kode_sys.h, kode_ao.h, kode_defs.h and kode_log.h are kode-sdk's public surface.
# A layer header of kode-sdk (kaos, kqp, kdrv, khal, kbsp, kui) here couples
# this SDK to that SDK's insides, which change between releases.
report "5. kode-sdk through its facade" \
  "Only the kode_*.h headers. Use what kode.h exposes; ask for it in kode-sdk if it does not." \
  "$(scan '#include "\(kaos\|kqp\|kdrv_\|khal_\|kbsp_\|kui_\)')"

# ---- 6. The layers do not leak what is below them ---------------------------
# What a task includes is camera.h and what an AO includes is camera_sys.h;
# neither names a driver or an owner type (the day a driver changes, no app
# has to), and neither includes the other: an app that uses both says so.
# Their types are camera_types.h's, once.
report "6. The layers do not leak what is below them" \
  "camera.h and camera_sys.h include nothing from camera_aos, camera_drv or each other. Give the layer its own type for what the app needs to see (camera_types.h)." \
  "$(grep -Hn '#include "camera_\(aos\|ao\.h\|plan\|recorder\|drv_\|sys\.h\)' \
         components/camera/include/*.h 2>/dev/null
     grep -Hn '#include "camera_\(aos\|ao\.h\|plan\|recorder\|drv_\)\|#include "camera\.h"' \
         components/camera_sys/include/*.h 2>/dev/null)"

# ---- 7. QP/C stays in the Active Objects ------------------------------------
# AO_Camera is camera_ao.c, and the app's AO talks to camera_sys.h. What its
# signals do (camera_aos.c), the drivers and the task layer see no QP/C type:
# the seam is camera_ao.h, and tests/host_owner plays the AO behind it.
report "7. QP/C stays in the Active Objects" \
  "kode_ao.h and kode_worker.h only in camera_ao.c; QActive and QEvt only in camera_ao.* and camera_sys.*. Put what needs QP/C behind a call in camera_ao.h." \
  "$({ scan '#include "\(kode_ao\|kode_worker\)\.h"' | grep -v '^components/camera_aos/camera/src/camera_ao\.c:'
       scan '\b\(QActive\|QEvt\|QACTIVE_\|Q_NEW\)' | grep -vE '^components/(camera_aos/camera/src/camera_ao\.[ch]|camera_sys/(include|src)/camera_sys\.[ch]):'; })"

# ---- 8. One name, one function ---------------------------------------------
# The components are static libraries: a function of the same name in two of
# them links without a word, and the callers of one get the other.
report "8. One name, one function" \
  "A function defined in two places. Give the lower layer's function its file's prefix (camera_drv_sensor_*, camera_aos_*...)." \
  "$(find components -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
       | xargs -0 grep -HoE '^[a-z_][a-z0-9_ *]*[ *]camera_[a-z0-9_]+\(' 2>/dev/null \
       | grep -v ':static' | sed -E 's/^([^:]+):.*[ *](camera_[a-z0-9_]+)\($/\2 \1/' \
       | sort | awk '{n[$1]++; f[$1]=f[$1]" "$2} END {for (k in n) if (n[k] > 1) print k ":" f[k]}')"

# ---- 9. One console name, one row ------------------------------------------
# kode-sdk's registry keeps a firmware's console functions and variables under
# one name each, [a-z0-9_] of 1..15 and never a function's and a variable's at
# once, each described in 48 characters at most. A table with one row against
# that is refused whole, and all that says so is a line on the log.
console_rows() {
    for example in examples/*/; do
        find "$example" -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
          | xargs -0 awk -v example="${example%/}" '
              /kode_(function|variable)_t[ \t]+[A-Za-z0-9_]+\[\][ \t]*=/ { rows = 1 }
              rows && /^\};/ { rows = 0 }
              rows {
                  line = $0
                  while (match(line, /\.name[ \t]*=[ \t]*"[^"]*"/)) {
                      name = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", name); sub(/"$/, "", name)
                      if (name !~ /^[a-z0-9_]+$/ || length(name) > 15)
                          print FILENAME ":" FNR ": \"" name "\" is not [a-z0-9_] of 1..15"
                      if (name in first)
                          print FILENAME ":" FNR ": \"" name "\" again, first at " first[name]
                      else
                          first[name] = FILENAME ":" FNR
                      line = substr(line, RSTART + RLENGTH)
                  }
                  line = $0
                  while (match(line, /\.desc[ \t]*=[ \t]*"[^"]*"/)) {
                      desc = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", desc); sub(/"$/, "", desc)
                      if (length(desc) > 48)
                          print FILENAME ":" FNR ": a description of " length(desc) " characters, 48 at most"
                      line = substr(line, RSTART + RLENGTH)
                  }
              }'
    done
}
report "9. One console name, one row" \
  "Rename or shorten the row: one bad row and kodeOS refuses its whole table." \
  "$(console_rows)"

# ---- 10. The target build links every call of both layers --------------------
# tests/target names each camera.h and camera_sys.h function in its ALL[] so
# that every one, and everything below it, is compiled and linked for the
# esp32p4 before it is first in somebody's app. A call added to a header and not
# there is not checked.
declared=$(grep -ohE '^[A-Za-z_][A-Za-z0-9_ ]*[ *]camera_[a-z0-9_]+\(' \
             components/camera/include/camera.h components/camera_sys/include/camera_sys.h \
           | sed -E 's/.*[ *](camera_[a-z0-9_]+)\($/\1/' | sort -u)
listed=$(grep -oE '\(const void \*\)camera_[a-z0-9_]+' tests/target/main/main.c \
         | sed -E 's/.*\)//' | sort -u)
report "10. The target build links every call of both layers (ALL[])" \
  "Add it to ALL[] in tests/target/main/main.c." \
  "$(comm -23 <(printf '%s\n' "$declared") <(printf '%s\n' "$listed"))"

# ---- 11. camera.h speaks no kode_status_t ------------------------------------
# The task layer answers bool and says why in camera_error(); a status code is
# the Active Object layer's language (camera_sys.h). Two ways to say one thing
# in one header is how an app ends up checking the wrong one.
report "11. camera.h speaks no kode_status_t" \
  "camera.h and camera.c: bool and camera_error(). A status code belongs to camera_sys.h." \
  "$(grep -Hn 'kode_status_t\|KODE_[A-Z_]*' components/camera/include/camera.h 2>/dev/null \
       | grep -vE ':[0-9]+: *(\*|//|/\*)' \
       | grep -v 'KODE_APP')"

# ---- 12. Every request in camera_sys.h takes the asker first -----------------
# A request is answered by a signal posted to `me`, and `me` is the first
# argument of each, as in kode_sys.h. A request without it has nowhere to
# answer. (camera_sys_photo_free() gives memory back and asks nothing.)
report "12. Every request in camera_sys.h takes QActive *me first" \
  "A function in the Requests section of camera_sys.h without 'QActive *me' as its first parameter." \
  "$(awk '/^\/\* ---- Requests/ { on = 1; next } /^\/\* ---- Immediates/ { on = 0 } \
          on && /^[A-Za-z_][A-Za-z0-9_ *]*[ *]camera_sys_[a-z0-9_]+\(/ \
              && !/camera_sys_[a-z0-9_]+\(QActive \*me[,)]/ && !/camera_sys_photo_free\(/ { print FILENAME ":" FNR ": " $0 }' \
         components/camera_sys/include/camera_sys.h)"

# ---- 13. kode_sys.h is the Active Object layer's and the owner's ------------
# Only camera_sys and camera_aos include it: the task layer reaches the owner
# through camera_aos.h, and the drivers reach nothing of the kernel.
report "13. Only camera_sys and camera_aos include kode_sys.h" \
  "The task layer reaches the owner through camera_aos.h; the drivers know no kernel service." \
  "$(scan '#include "kode_sys\.h"' 'components/(camera_sys|camera_aos)/')"

# ---- 14. The Active Object layer never waits ---------------------------------
# camera_sys.c answers at once or posts: a wait in a step of an AO stops every
# AO on its queue. No semaphore, no delay, no wait on the owner (camera_aos_call
# is the task layer's door), and no include of the task layer.
report "14. The Active Object layer never waits" \
  "camera_sys.c blocks or includes camera.h: use a request, whose answer arrives as a signal." \
  "$(grep -HnE 'xSemaphore(Take|Give)|vTaskDelay|vTaskDelayUntil|xQueueReceive|xEventGroupWaitBits|ulTaskNotifyTake|camera_aos_call|camera_aos_wait|#include "camera\.h"' \
         components/camera_sys/src/camera_sys.c 2>/dev/null \
       | grep -vE ':[0-9]+: *(\*|//|/\*)')"

exit "$fail"
