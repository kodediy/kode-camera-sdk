/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera.c
 * @brief See fake_camera.h. An argument the layer refuses is refused here with KODE_DENIED (camera_sys.c's own
 *        answer) and counted, never taken; so is a request whose door is open.
 */
#include "fake_camera.h"

#include <stdio.h>
#include <string.h>

fake_camera_t g_camera;

void fake_camera_reset(void)
{
    (void)memset(&g_camera, 0, sizeof(g_camera));
    g_camera.answer           = KODE_OK;
    g_camera.subscribe_answer = KODE_OK;
    g_camera.st.state         = CAMERA_RUNNING;
    g_camera.focus.mode       = CAMERA_FOCUS_AUTO;
    g_camera.focus.state      = CAMERA_FOCUS_IDLE;
    g_camera.focus.lens       = CAMERA_LENS_REST;
}

void fake_camera_door_close(fake_door_t door)
{
    g_camera.door[door] = false;
}

/** @brief Begin a call: counted, and the record cleared for it. */
static void begin(camera_call_t call)
{
    g_camera.calls++;
    (void)memset(&g_camera.last, 0, sizeof(g_camera.last));
    g_camera.last.call = call;
}

static kode_status_t bad(void)
{
    g_camera.bad_args++;
    return KODE_DENIED;
}

static bool path_ok(const char *path)
{
    return (path != NULL) && (path[0] != '\0') && (strlen(path) <= PATH_LIMIT);
}

static void keep_path(const char *path)
{
    g_camera.last.path_given = path;
    (void)snprintf(g_camera.last.path, sizeof(g_camera.last.path), "%s", path);
}

/**
 * @brief Take a request of @p me through @p door: refused (DENIED, no `me` or an open door), the case's answer, or taken.
 * @return What camera_sys_<verb>() answers.
 */
static kode_status_t ask(fake_door_t door, QActive *me)
{
    g_camera.requests++;
    g_camera.last.me = me;
    if (me == NULL) {
        return bad();
    }
    if (g_camera.door[door]) {
        g_camera.busy++;
        return KODE_DENIED;
    }
    if (g_camera.answer == KODE_OK) {
        g_camera.door[door] = true;
    }
    return g_camera.answer;
}

const char *camera_sys_state_name(camera_state_t state)
{
    switch (state) {
        case CAMERA_OFF:          return "off";
        case CAMERA_HEADER_BUSY:  return "header busy";
        case CAMERA_LOOKING:      return "looking";
        case CAMERA_NO_SENSOR:    return "no sensor";
        case CAMERA_WRONG_SENSOR: return "wrong sensor";
        case CAMERA_STARTING:     return "starting";
        case CAMERA_RUNNING:      return "running";
        case CAMERA_RESTARTING:   return "restarting";
        case CAMERA_STANDBY:      return "standby";
        case CAMERA_NO_MEMORY:    return "no memory";
        default:                  return "?";
    }
}

const char *camera_sys_status_text(kode_status_t rc)
{
    static char text[24];
    (void)snprintf(text, sizeof(text), "status %d", (int)rc);
    return text;
}

kode_status_t camera_sys_subscribe(QActive *me)
{
    g_camera.subscribed++;
    g_camera.subscriber = me;
    return (me == NULL) ? KODE_DENIED : g_camera.subscribe_answer;
}

kode_status_t camera_sys_status_read(camera_status_t *out)
{
    g_camera.reads++;
    if (out == NULL) {
        return KODE_DENIED;
    }
    *out = g_camera.st;
    return KODE_OK;
}

void camera_sys_focus_info(camera_focus_info_t *out)
{
    g_camera.reads++;
    *out = g_camera.focus;
}

void camera_sys_video_info(camera_video_info_t *out)
{
    g_camera.reads++;
    *out = g_camera.video;
}

kode_status_t camera_sys_photo_save(QActive *me, const char *path, const camera_photo_options_t *shot)
{
    begin(CAMERA_CALL_PHOTO_SAVE);
    if (!path_ok(path) || ((shot != NULL) && (shot->quality > 100U))) {
        g_camera.requests++;
        return bad();
    }
    keep_path(path);
    if (shot != NULL) {
        g_camera.last.shot     = *shot;
        g_camera.last.has_shot = true;
    }
    return ask(DOOR_SAVE, me);
}

kode_status_t camera_sys_focus(QActive *me, uint32_t ms)
{
    begin(CAMERA_CALL_FOCUS);
    g_camera.last.ms = ms;
    return ask(DOOR_FOCUS, me);
}

kode_status_t camera_sys_focus_mode(camera_focus_mode_t mode)
{
    begin(CAMERA_CALL_FOCUS_MODE);
    if ((unsigned)mode > (unsigned)CAMERA_FOCUS_MANUAL) {
        return bad();
    }
    g_camera.last.mode = mode;
    return g_camera.answer;
}

kode_status_t camera_sys_focus_at(uint16_t lens)
{
    begin(CAMERA_CALL_FOCUS_AT);
    if (lens > CAMERA_LENS_NEAR) {
        return bad();
    }
    g_camera.last.lens = lens;
    return g_camera.answer;
}

kode_status_t camera_sys_standby(bool on)
{
    begin(CAMERA_CALL_STANDBY);
    g_camera.last.on = on;
    return g_camera.answer;
}

/** @brief camera_sys.h's range for each setting the example reaches; the rest are not its rows. */
static bool setting_ok(camera_setting_t s, int32_t v)
{
    switch (s) {
        case CAMERA_WHITE_BALANCE:         return (v >= 0) && (v <= (int32_t)CAMERA_WHITE_BALANCE_NEUTRAL);
        case CAMERA_EXPOSURE_COMPENSATION: return (v >= -3) && (v <= 3);
        case CAMERA_BRIGHTNESS:            return (v >= -3) && (v <= 3);
        case CAMERA_CONTRAST:              return (v >= -3) && (v <= 3);
        case CAMERA_SATURATION:            return (v >= -4) && (v <= 4);
        case CAMERA_EFFECT:                return (v >= 0) && (v <= (int32_t)CAMERA_EFFECT_SEPIA);
        default:                           return false;
    }
}

kode_status_t camera_sys_set(camera_setting_t s, int32_t value)
{
    begin(CAMERA_CALL_SET);
    if (!setting_ok(s, value)) {
        return bad();
    }
    g_camera.last.setting = s;
    g_camera.last.value   = value;
    return g_camera.answer;
}

kode_status_t camera_sys_settings_reset(void)
{
    begin(CAMERA_CALL_SETTINGS_RESET);
    return g_camera.answer;
}

kode_status_t camera_sys_video_start(QActive *me, const char *path, const camera_video_options_t *opts)
{
    begin(CAMERA_CALL_VIDEO_START);
    if (!path_ok(path)) {
        g_camera.requests++;
        return bad();
    }
    keep_path(path);
    if (opts != NULL) {
        g_camera.last.movie     = *opts;
        g_camera.last.has_movie = true;
    }
    return ask(DOOR_VIDEO_START, me);
}

kode_status_t camera_sys_video_stop(QActive *me)
{
    begin(CAMERA_CALL_VIDEO_STOP);
    return ask(DOOR_VIDEO_STOP, me);
}
