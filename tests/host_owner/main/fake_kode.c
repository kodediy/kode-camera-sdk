/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kode.c
 * @brief kode.h's calls the SDK makes, on the host: the clock is FreeRTOS's,
 *        J3 is lent as FAKE_GPIO0 + position, the files are the host's (a
 *        test passes a path in a folder of its own) and the microphone is a
 *        440 Hz tone.
 */
#include "kode_sys.h"
#include "kode.h"
#include "camera_ao.h"

#include <errno.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fake_sensor.h"

void kode_init(void)
{
    g_fake.inits++;
}

uint32_t kode_sys_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + g_fake.clock_offset;
}

kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag, uint8_t *gpio)
{
    (void)tag;
    if (g_fake.j3_busy) {
        return KODE_DENIED;
    }
    for (size_t i = 0U; i < n; i++) {
        gpio[i] = (uint8_t)(FAKE_GPIO0 + pos[i]);
    }
    g_fake.lends++;
    return KODE_OK;
}

kode_status_t kode_sys_exp_return(const char *tag)
{
    (void)tag;
    g_fake.returns++;
    return KODE_OK;
}

kode_status_t kode_sys_exp_power(bool on)
{
    if (on) {
        g_fake.power_on_calls++;
    } else {
        g_fake.power_off_calls++;
    }
    if (g_fake.power_denied) {
        return KODE_DENIED;
    }
    g_fake.powered = on;
    return KODE_OK;
}

kode_status_t kode_sys_home_defer(void)
{
    g_fake.home_defers++;
    return KODE_OK;
}

kode_status_t kode_sys_home_now(void)
{
    g_fake.home_nows++;
    return KODE_OK;
}

/* The worker's proxies (camera_ao.h): on the board they go through AO_Storage and AO_Audio, one
 * request in flight each; here they run on the caller's task over the host's files and a tone. */

kode_status_t camera_ao_audio_record(int16_t *out, size_t cap, uint32_t ms, size_t *n)
{
    size_t const want = (size_t)ms * 48U;
    size_t const k    = (want < cap) ? want : cap;
    for (size_t i = 0U; i < k; i++) {
        out[i] = (int16_t)(8000.0 * sin((2.0 * M_PI * 440.0 * (double)i) / 48000.0));
    }
    if (g_fake.audio_fail) {
        if (n != NULL) {
            *n = 0U;
        }
        return KODE_IO_ERROR;   /* at once: the codec is held by someone else */
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (n != NULL) {
        *n = k;
    }
    return KODE_OK;
}

kode_status_t camera_ao_file_status(const char *path, bool *exists, bool *is_dir, uint32_t *size, uint32_t ms)
{
    struct stat st;
    (void)ms;

    g_fake.fs_stats++;
    (void)snprintf(g_fake.fs_stat_task, sizeof(g_fake.fs_stat_task), "%s", pcTaskGetName(NULL));
    if (path == NULL) {
        return KODE_DENIED;
    }
    if (g_fake.fs_down) {
        return KODE_IO_ERROR;   /* no card */
    }
    bool e = false;
    bool d = false;
    uint32_t sz = 0U;
    if (stat(path, &st) == 0) {
        e  = true;
        d  = S_ISDIR(st.st_mode);
        sz = d ? 0U : (uint32_t)st.st_size;
    }
    if (exists != NULL) {
        *exists = e;
    }
    if (is_dir != NULL) {
        *is_dir = d;
    }
    if (size != NULL) {
        *size = sz;
    }
    return KODE_OK;
}

kode_status_t camera_ao_file_delete(const char *path, uint32_t ms)
{
    (void)ms;
    if (g_fake.fs_down) {
        return KODE_IO_ERROR;
    }
    return (remove(path) == 0) ? KODE_OK : KODE_ERROR;
}

kode_status_t camera_ao_file_rename(const char *from, const char *to, uint32_t ms)
{
    (void)ms;
    if (g_fake.fs_down || g_fake.rename_fail) {
        return KODE_IO_ERROR;
    }
    return (rename(from, to) == 0) ? KODE_OK : KODE_ERROR;
}

kode_status_t camera_ao_file_read(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *got, uint32_t ms)
{
    (void)ms;
    if (g_fake.fs_down) {
        return KODE_IO_ERROR;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return KODE_DENIED;
    }
    (void)fseek(f, (long)offset, SEEK_SET);
    size_t const n = fread(buf, 1U, cap, f);
    (void)fclose(f);
    if (got != NULL) {
        *got = (uint32_t)n;
    }
    return KODE_OK;
}

kode_status_t camera_ao_file_write(const char *path, const void *data, uint32_t len, bool append, uint32_t ms)
{
    (void)ms;
    if (g_fake.fs_down || (g_fake.file_write_fail && !append)) {
        return KODE_IO_ERROR;
    }
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (f == NULL) {
        return KODE_DENIED;
    }
    bool const ok = fwrite(data, 1U, len, f) == len;
    return ((fclose(f) == 0) && ok) ? KODE_OK : KODE_IO_ERROR;
}

volatile uint32_t g_fake_slow_callback_logs;

void fake_klog_warned(const char *fmt, ...)
{
    char line[200];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (strstr(line, "event callback took") != NULL) {
        g_fake_slow_callback_logs++;
    }
}
