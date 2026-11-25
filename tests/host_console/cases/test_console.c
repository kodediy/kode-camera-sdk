/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console.c
 * @brief examples/console_actions over a recording camera_sys.h: its tables
 *        kept to kodeOS's rules, every function making the camera_sys.h call
 *        its name says with the limits of its argument, every request
 *        answered by a signal, every variable a word the console sends, in
 *        every state the camera can be in.
 *
 * The example is built into this file, so a case carries a request as the
 * board does: the row posts it (fake_console_run()), the Active Object's one
 * state takes it and makes the camera_sys.h call (serve()), and for a request
 * the owner's answer is posted back to the AO (answer_*()), which logs what
 * came of it. Nothing waits: the example links camera_sys.h alone, and there
 * is no camera.h here for it to wait with.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fake_camera.h"
#include "fake_console.h"
#include "fake_kode.h"

#include "console_actions_ao.c"

#define FUNCTION_COUNT 15U
#define VARIABLE_COUNT 10U

/* The queue rule: a slot for each door, each piece of news the AO subscribes to, and the console's own events. */
_Static_assert(CONSOLE_NEWS >= (unsigned)(CAMERA_PHOTO_TAKEN_SIG - CAMERA_SIG_FIRST) + 1U,
               "every piece of news has a slot");

/** @brief The example's rows, as its header lists them. */
static const char *const FUNCTION_NAME[FUNCTION_COUNT] = {
    "cam_photo", "cam_big_photo", "cam_focus", "cam_focus_mode", "cam_lens", "cam_standby",
    "cam_balance", "cam_effect", "cam_brightness", "cam_contrast", "cam_saturation",
    "cam_exposure_ev", "cam_video_start", "cam_video_stop", "cam_defaults",
};

static const char *const VARIABLE_NAME[VARIABLE_COUNT] = {
    "cam_state", "cam_fps", "cam_frames", "cam_restarts", "cam_rotated", "cam_lens_now",
    "cam_focus_now", "cam_video", "cam_big_now", "cam_last",
};

/** @brief Functions on the console when the initial transition ran. */
static size_t s_functions_at_initial;

/** @brief What QACTIVE_START() runs, where QP/C takes the initial transition. */
static void initial(void)
{
    s_functions_at_initial = fake_console_functions();
    TEST_ASSERT_EQUAL_UINT8(Q_TRAN(&ConsoleActions_serving), ConsoleActions_initial(&l_ao, NULL));
}

/**
 * @brief Nothing recorded, a camera running, every answer KODE_OK, and the
 *        example's own state as a fresh boot has it (statics of the file this
 *        one includes, so the previous case's would otherwise stay).
 */
static void fakes_reset(void)
{
    fake_kode_reset();
    fake_camera_reset();
    fake_console_reset();
    g_kode.initial = initial;
    (void)memset(&l_ao, 0, sizeof(l_ao));
    l_last = 0U;
}

/** @brief The example brought up as its main() does. */
static void console_up(void)
{
    ConsoleActions_ctor();
    ConsoleActions_start();
}

static void console_reset(void)
{
    fakes_reset();
    console_up();
}

/** @brief The AO's step for each event posted to it, oldest first, each block then freed. */
static void serve(void)
{
    QEvt const *e;
    while ((e = fake_kode_take()) != NULL) {
        TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, e));
        fake_kode_free(e);
    }
}

/** @brief The answer of a request that needs nothing but a status: given to the AO as the owner posts it. */
static void answer_with(QSignal sig, fake_door_t door, kode_status_t rc, const char *why)
{
    camera_sys_answer_t a = { .super = { .sig = sig }, .rc = rc, .why = why };
    fake_camera_door_close(door);   /* the layer opens the door just before it posts */
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &a.super));
}

static void answer_save(kode_status_t rc, const char *why)
{
    answer_with((QSignal)CAMERA_PHOTO_SAVE_RSP_SIG, DOOR_SAVE, rc, why);
}

static void answer_video_start(kode_status_t rc, const char *why)
{
    answer_with((QSignal)CAMERA_VIDEO_START_RSP_SIG, DOOR_VIDEO_START, rc, why);
}

static void answer_video_stop(kode_status_t rc, const char *why)
{
    answer_with((QSignal)CAMERA_VIDEO_STOP_RSP_SIG, DOOR_VIDEO_STOP, rc, why);
}

static void answer_focus(kode_status_t rc, const char *why, bool focused)
{
    camera_sys_focus_answer_t a = {
        .super = { .sig = (QSignal)CAMERA_FOCUS_RSP_SIG }, .rc = rc, .why = why, .focused = focused,
    };
    fake_camera_door_close(DOOR_FOCUS);
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &a.super));
}

/** @brief Every request that is open answered KODE_OK, as the owner does once it has done it. */
static void answer_all_ok(void)
{
    if (g_camera.door[DOOR_SAVE]) {
        answer_save(KODE_OK, NULL);
    }
    if (g_camera.door[DOOR_FOCUS]) {
        answer_focus(KODE_OK, NULL, true);
    }
    if (g_camera.door[DOOR_VIDEO_START]) {
        answer_video_start(KODE_OK, NULL);
    }
    if (g_camera.door[DOOR_VIDEO_STOP]) {
        answer_video_stop(KODE_OK, NULL);
    }
}

/** @brief One piece of news, given to the AO as the owner posts it. */
static void news(int sig, int32_t value)
{
    camera_sys_news_t n = { .super = { .sig = (QSignal)sig }, .value = value };
    TEST_ASSERT_EQUAL_UINT8(Q_HANDLED(), ConsoleActions_serving(&l_ao, &n.super));
}

static kode_value_t integer(int32_t i)
{
    kode_value_t v = { 0 };
    v.i = i;
    return v;
}

static kode_value_t boolean(bool b)
{
    kode_value_t v = { 0 };
    v.b = b;
    return v;
}

static kode_value_t word(uint8_t e)
{
    kode_value_t v = { 0 };
    v.e = e;
    return v;
}

static kode_value_t text(const char *s)
{
    kode_value_t v = { 0 };
    v.s = s;
    return v;
}

/**
 * @brief `function name=@p name` with @p v, carried out: the row makes no call of the camera's, the AO's step
 *        makes the one @p call, camera_sys.h refuses none of its arguments, and a request that was taken is
 *        answered KODE_OK.
 */
static void asks(const char *name, const kode_value_t *v, camera_call_t call)
{
    unsigned const before = g_camera.calls;
    unsigned const bad    = g_camera.bad_args;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_run(name, v), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, g_camera.calls, name);   /* the row only posts */
    serve();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_camera.calls, name);
    TEST_ASSERT_EQUAL_MESSAGE(call, g_camera.last.call, name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(bad, g_camera.bad_args, name);
    answer_all_ok();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), name);
}

/** @brief `function name=@p name` refused before anything is posted or called. */
static void refused(const char *name, const kode_value_t *v)
{
    unsigned const calls = g_camera.calls;
    TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(name, v), name);
    TEST_ASSERT_NULL_MESSAGE(fake_kode_take(), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls, g_camera.calls, name);
}

/** @brief Read variable @p name: one word of 1 to 32 characters, with no space in it. */
static void reads(const char *name, const char *want)
{
    char out[KODE_VALUE_STR_MAX + 1U];
    TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_read(name, out), name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out, name);
    TEST_ASSERT_NULL_MESSAGE(strpbrk(out, " \t\r\n"), name);
}

/** @brief Every variable gives a word the console would send, whatever the state. */
static void all_variables_read(void)
{
    char out[KODE_VALUE_STR_MAX + 1U];
    for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_read(VARIABLE_NAME[k], out), VARIABLE_NAME[k]);
        TEST_ASSERT_NULL_MESSAGE(strpbrk(out, " \t\r\n"), VARIABLE_NAME[k]);
    }
}

/** @brief What cam_last says for @p kind (the name) and @p status. */
static void last_is(const char *kind, kode_status_t status)
{
    char want[KODE_VALUE_STR_MAX + 1U];
    (void)snprintf(want, sizeof(want), "%s:%d", kind, (int)status);
    reads("cam_last", want);
}

/** @brief The log's last line is the refusal of @p name with @p status. */
static void logged_status(const char *name, kode_status_t status)
{
    char want[FAKE_KODE_LINE];
    (void)snprintf(want, sizeof(want), "%s: refused (status %d)", name, (int)status);
    TEST_ASSERT_EQUAL_STRING(want, fake_kode_last_line());
}

/** @brief Whether @p s is [a-z0-9_] and 1 to KODE_ROW_NAME_MAX long: a row's name or an enum's word. */
static bool is_name(const char *s)
{
    size_t const n = strlen(s);
    if ((n == 0U) || (n > KODE_ROW_NAME_MAX)) {
        return false;
    }
    for (size_t k = 0U; k < n; k++) {
        char const c = s[k];
        if (!(((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_'))) {
            return false;
        }
    }
    return true;
}

/* ---- Registration -------------------------------------------------------- */

TEST_CASE("ConsoleActions_start() gives the console fifteen functions and ten variables, once", "[console]")
{
    console_reset();
    TEST_ASSERT_EQUAL_UINT(0U, s_functions_at_initial);   /* the news first, then the rows */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
    TEST_ASSERT_TRUE(fake_kode_logged("up; the camera's rows are on the console"));

    /* Nothing is asked of the camera by coming up: nothing records, nothing is photographed. It asks for its news. */
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
    TEST_ASSERT_EQUAL_UINT(1U, g_camera.subscribed);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_camera.subscriber);
    TEST_ASSERT_NULL(fake_kode_take());

    /* Its own task and queue, at the app's level; no worker. The queue is the rule's: a slot for each door, the
     * news and the console's events. */
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.starts);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_kode.ao);
    TEST_ASSERT_EQUAL_STRING("console_actions", g_kode.task_name);
    TEST_ASSERT_EQUAL_UINT8(KODE_PRIO_USER_LOW, g_kode.prio);
    TEST_ASSERT_EQUAL_UINT(CAMERA_SYS_DOORS + CONSOLE_NEWS + CONSOLE_OWN, g_kode.queue_len);
    TEST_ASSERT_EQUAL_UINT(KQP_AO_STACK_BYTES, g_kode.stack_bytes);

    /* The names the header lists are the names on the console, and they read. */
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(fake_console_function(FUNCTION_NAME[k]), FUNCTION_NAME[k]);
    }
    all_variables_read();
    last_is("none", 0);

    g_kode.initial = NULL;
    ConsoleActions_start();   /* the names are taken: both tables refused whole */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    TEST_ASSERT_TRUE(fake_kode_logged("the console refused the functions"));
    TEST_ASSERT_TRUE(fake_kode_logged("the console refused the variables"));
    TEST_ASSERT_EQUAL_UINT(2U, g_kode.errors);
}

TEST_CASE("both tables keep kodeOS's registry rules, on their own and not only through the registry", "[console]")
{
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, sizeof(FUNCTIONS) / sizeof(FUNCTIONS[0]));
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, sizeof(VARIABLES) / sizeof(VARIABLES[0]));

    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        const kode_function_t *const f = &FUNCTIONS[k];
        TEST_ASSERT_EQUAL_STRING(FUNCTION_NAME[k], f->name);
        TEST_ASSERT_TRUE_MESSAGE(is_name(f->name), f->name);
        TEST_ASSERT_TRUE_MESSAGE(strncmp(f->name, "cam_", 4U) == 0, f->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(f->desc, f->name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(f->desc) <= KODE_ROW_DESC_MAX, f->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(f->run, f->name);
        TEST_ASSERT_TRUE_MESSAGE((unsigned)f->arg <= (unsigned)KODE_TYPE_STRING, f->name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, f->answer_ms, f->name);
        if (f->arg == KODE_TYPE_INT) {
            TEST_ASSERT_TRUE_MESSAGE(f->min < f->max, f->name);   /* the console only checks a range when it is one */
        }
        if (f->arg == KODE_TYPE_ENUM) {
            TEST_ASSERT_TRUE_MESSAGE((f->n_enums >= 1U) && (f->n_enums <= KODE_VALUE_ENUMS_MAX), f->name);
            for (uint8_t w = 0U; w < f->n_enums; w++) {
                TEST_ASSERT_TRUE_MESSAGE(is_name(f->enums[w]), f->enums[w]);
            }
        }
        for (size_t j = 0U; j < k; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(f->name, FUNCTIONS[j].name) != 0, f->name);
        }
    }
    for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
        const kode_variable_t *const v = &VARIABLES[k];
        TEST_ASSERT_EQUAL_STRING(VARIABLE_NAME[k], v->name);
        TEST_ASSERT_TRUE_MESSAGE(is_name(v->name), v->name);
        TEST_ASSERT_TRUE_MESSAGE(strncmp(v->name, "cam_", 4U) == 0, v->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(v->desc, v->name);
        TEST_ASSERT_TRUE_MESSAGE(strlen(v->desc) <= KODE_ROW_DESC_MAX, v->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(v->get, v->name);
        for (size_t j = 0U; j < k; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(v->name, VARIABLES[j].name) != 0, v->name);
        }
        /* One namespace for both kinds. */
        for (size_t j = 0U; j < FUNCTION_COUNT; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(v->name, FUNCTIONS[j].name) != 0, v->name);
        }
    }
}

static kode_status_t app_get(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return KODE_OK;
}

static kode_status_t app_run(const kode_value_t *arg)
{
    (void)arg;
    return KODE_OK;
}

TEST_CASE("an app's row named like one of the example's is refused, whichever its kind", "[console]")
{
    console_reset();
    static const kode_variable_t SAME_VARIABLE[] = {
        { .name = "cam_state", .get = app_get, .desc = "the app's" },
    };
    static const kode_function_t SAME_FUNCTION[] = {
        { .name = "cam_defaults", .run = app_run, .desc = "the app's" },
    };
    static const kode_function_t FUNCTION_AS_VARIABLE[] = {
        { .name = "cam_last", .run = app_run, .desc = "the app's" },
    };
    static const kode_variable_t VARIABLE_AS_FUNCTION[] = {
        { .name = "cam_photo", .get = app_get, .desc = "the app's" },
    };
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(SAME_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(SAME_FUNCTION));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(FUNCTION_AS_VARIABLE));
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_VARIABLES_ADD(VARIABLE_AS_FUNCTION));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());

    /* One bad row refuses the table whole: the good row before it is not kept,
     * so the name is still free to be added alone. */
    static const kode_function_t GOOD_THEN_TAKEN[] = {
        { .name = "app_function", .run = app_run, .desc = "the app's" },
        { .name = "cam_focus", .run = app_run, .desc = "the app's" },
    };
    static const kode_function_t GOOD_ALONE[] = {
        { .name = "app_function", .run = app_run, .desc = "the app's" },
    };
    TEST_ASSERT_EQUAL(KODE_EXISTS, KODE_FUNCTIONS_ADD(GOOD_THEN_TAKEN));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL(KODE_OK, KODE_FUNCTIONS_ADD(GOOD_ALONE));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT + 1U, fake_console_functions());
}

TEST_CASE("the console's room is left to the app: 15 of 64 functions, 10 of 32 variables", "[console]")
{
    console_reset();
    /* 22 more variables fit, the 23rd does not: the example may not take more. */
    static const char *const SPARE[23] = {
        "app_v0",  "app_v1",  "app_v2",  "app_v3",  "app_v4",  "app_v5",  "app_v6",  "app_v7",
        "app_v8",  "app_v9",  "app_v10", "app_v11", "app_v12", "app_v13", "app_v14", "app_v15",
        "app_v16", "app_v17", "app_v18", "app_v19", "app_v20", "app_v21", "app_v22",
    };
    static kode_variable_t table[23];
    for (size_t k = 0U; k < 23U; k++) {
        table[k] = (kode_variable_t){ .name = SPARE[k], .get = app_get, .desc = "the app's" };
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kode_variables_add(table, 23U));
    TEST_ASSERT_EQUAL(KODE_OK, kode_variables_add(table, 22U));
    TEST_ASSERT_EQUAL_UINT(KODE_VARIABLES_MAX, fake_console_variables());
}

/** @brief Row @p name takes an int from @p lo to @p hi, as function_list will say. */
static void range_is(const char *name, int32_t lo, int32_t hi)
{
    const kode_function_t *const r = fake_console_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_INT, r->arg, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(lo, r->min, name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(hi, r->max, name);
}

/** @brief Row @p name takes one of the @p n words listed; one past them, or none, is refused. */
static void words_are(const char *name, uint8_t n, const char *const *list)
{
    const kode_function_t *const r = fake_console_function(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, name);
    TEST_ASSERT_EQUAL_MESSAGE(KODE_TYPE_ENUM, r->arg, name);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(n, r->n_enums, name);
    for (uint8_t k = 0U; k < n; k++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(list[k], r->enums[k], name);
    }
    kode_value_t v = word(n);
    refused(name, &v);
    refused(name, NULL);   /* the line carried no arg=: the row refuses it */
}

TEST_CASE("the rows ask what the header says: types, ranges and words", "[console]")
{
    console_reset();

    range_is("cam_lens", 0, 1023);
    range_is("cam_effect", 0, 6);
    range_is("cam_brightness", -3, 3);
    range_is("cam_contrast", -3, 3);
    range_is("cam_saturation", -4, 4);
    range_is("cam_exposure_ev", -3, 3);

    static const char *const FOCUS_WORD[]   = { "off", "auto", "manual" };
    static const char *const BALANCE_WORD[] = { "auto", "sunny", "cloudy", "office", "home", "neutral" };
    words_are("cam_focus_mode", 3U, FOCUS_WORD);
    words_are("cam_balance", 6U, BALANCE_WORD);

    TEST_ASSERT_EQUAL(KODE_TYPE_BOOL, fake_console_function("cam_standby")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_STRING, fake_console_function("cam_photo")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_STRING, fake_console_function("cam_big_photo")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_STRING, fake_console_function("cam_video_start")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("cam_focus")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("cam_video_stop")->arg);
    TEST_ASSERT_EQUAL(KODE_TYPE_NONE, fake_console_function("cam_defaults")->arg);

    /* No row waits for an answer: each one is `>ok` once accepted. */
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, fake_console_function(FUNCTION_NAME[k])->answer_ms,
                                         FUNCTION_NAME[k]);
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
}

/* ---- Calls --------------------------------------------------------------- */

/** @brief One row, the argument that makes it, the call it ends in and its name in cam_last. */
typedef struct {
    const char    *name;
    kode_value_t   value;
    bool           has_value;
    camera_call_t  call;
    const char    *kind;
    const char    *done;      /**< The log line of its success. */
    bool           request;   /**< A request: it is answered by a signal; else a command. */
} row_t;

static const row_t ROWS[FUNCTION_COUNT] = {
    { "cam_photo",       { .s = "/sdcard/a.jpg" }, true,  CAMERA_CALL_PHOTO_SAVE,     "photo",       "cam_photo: done, /sdcard/a.jpg", true },
    { "cam_big_photo",   { .s = "/sdcard/b.jpg" }, true,  CAMERA_CALL_PHOTO_SAVE,     "big_photo",   "cam_big_photo: done, /sdcard/b.jpg", true },
    { "cam_focus",       { 0 },                    false, CAMERA_CALL_FOCUS,          "focus",       "cam_focus: done", true },
    { "cam_focus_mode",  { .e = 1U },              true,  CAMERA_CALL_FOCUS_MODE,     "focus_mode",  "cam_focus_mode: done", false },
    { "cam_lens",        { .i = 600 },             true,  CAMERA_CALL_FOCUS_AT,       "lens",        "cam_lens: done", false },
    { "cam_standby",     { .b = true },            true,  CAMERA_CALL_STANDBY,        "standby",     "cam_standby: done", false },
    { "cam_balance",     { .e = 4U },              true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_effect",      { .i = 6 },               true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_brightness",  { .i = -3 },              true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_contrast",    { .i = 3 },               true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_saturation",  { .i = -4 },              true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_exposure_ev", { .i = 2 },               true,  CAMERA_CALL_SET,            "set",         "a setting: done", false },
    { "cam_video_start", { .s = "/sdcard/m.mov" }, true,  CAMERA_CALL_VIDEO_START,    "video_start", "cam_video_start: done, /sdcard/m.mov", true },
    { "cam_video_stop",  { 0 },                    false, CAMERA_CALL_VIDEO_STOP,     "video_stop",  "cam_video_stop: done", true },
    { "cam_defaults",    { 0 },                    false, CAMERA_CALL_SETTINGS_RESET, "defaults",    "cam_defaults: done", false },
};

TEST_CASE("each of the fifteen functions ends in the camera_sys.h call its name says, a command or a request, and cam_last names it", "[console]")
{
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        console_reset();
        TEST_ASSERT_EQUAL_STRING(FUNCTION_NAME[k], ROWS[k].name);
        asks(ROWS[k].name, ROWS[k].has_value ? &ROWS[k].value : NULL, ROWS[k].call);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, g_camera.calls, ROWS[k].name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ROWS[k].done, fake_kode_last_line(), ROWS[k].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_kode.warnings, ROWS[k].name);
        last_is(ROWS[k].kind, KODE_OK);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_kode.errors, ROWS[k].name);
    }
}

TEST_CASE("the row makes no camera_sys.h call: the AO's step makes the one; a request is answered by a signal, a command is not", "[console]")
{
    console_reset();
    unsigned requests = 0U;
    for (size_t k = 0U; k < FUNCTION_COUNT; k++) {
        unsigned const before = g_camera.calls;
        unsigned const asked  = g_camera.requests;
        TEST_ASSERT_EQUAL_MESSAGE(KODE_OK,
                                  fake_console_run(ROWS[k].name, ROWS[k].has_value ? &ROWS[k].value : NULL),
                                  ROWS[k].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(before, g_camera.calls, ROWS[k].name);
        serve();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, g_camera.calls, ROWS[k].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(ROWS[k].request ? 1U : 0U, g_camera.requests - asked, ROWS[k].name);
        if (ROWS[k].request) {
            requests++;
            /* The line "done" is not on the log until the answer: a request taken says nothing yet. */
            TEST_ASSERT_FALSE_MESSAGE(fake_kode_logged(ROWS[k].done), ROWS[k].name);
            answer_all_ok();
        }
        TEST_ASSERT_TRUE_MESSAGE(fake_kode_logged(ROWS[k].done), ROWS[k].name);
    }
    TEST_ASSERT_EQUAL_UINT(5U, requests);   /* photo, big photo, focus, video start, video stop */
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, g_camera.calls);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.bad_args);
}

TEST_CASE("cam_photo takes the preview at the default quality to the path; cam_big_photo 5 MP, focused, at 90", "[console]")
{
    console_reset();

    kode_value_t v = text("/sdcard/a.jpg");
    asks("cam_photo", &v, CAMERA_CALL_PHOTO_SAVE);
    TEST_ASSERT_EQUAL_STRING("/sdcard/a.jpg", g_camera.last.path);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_camera.last.me);   /* the answer comes back to the AO */
    TEST_ASSERT_FALSE(g_camera.last.has_shot);                    /* NULL: a preview photo at quality 90 */

    v = text("/sdcard/b.jpg");
    asks("cam_big_photo", &v, CAMERA_CALL_PHOTO_SAVE);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_camera.last.me);
    TEST_ASSERT_EQUAL_STRING("/sdcard/b.jpg", g_camera.last.path);
    TEST_ASSERT_TRUE(g_camera.last.has_shot);
    TEST_ASSERT_EQUAL(CAMERA_RESOLUTION_5MP, g_camera.last.shot.resolution);
    TEST_ASSERT_EQUAL_UINT8(90U, g_camera.last.shot.quality);
    TEST_ASSERT_TRUE(g_camera.last.shot.focus);
    /* Everything else is camera_sys.h's default: the resolution's own size, upright, with EXIF, no zoom. */
    TEST_ASSERT_EQUAL_UINT16(0U, g_camera.last.shot.width);
    TEST_ASSERT_EQUAL_UINT16(0U, g_camera.last.shot.height);
    TEST_ASSERT_FALSE(g_camera.last.shot.mirror);
    TEST_ASSERT_EQUAL_UINT16(0U, g_camera.last.shot.zoom_x16);
    TEST_ASSERT_FALSE(g_camera.last.shot.no_exif);
    TEST_ASSERT_NULL(g_camera.last.shot.gps);
}

TEST_CASE("a path is something, and at most 96 characters; the row refuses the rest before it posts", "[console]")
{
    console_reset();
    char longest[CONSOLE_PATH_MAX + 2U];
    (void)memset(longest, 'a', sizeof(longest));
    longest[0] = '/';
    longest[CONSOLE_PATH_MAX]      = '\0';   /* 96 characters */
    longest[CONSOLE_PATH_MAX + 1U] = '\0';

    static const char *const PATH_ROWS[] = { "cam_photo", "cam_big_photo", "cam_video_start" };
    static const camera_call_t PATH_CALL[] = { CAMERA_CALL_PHOTO_SAVE, CAMERA_CALL_PHOTO_SAVE,
                                               CAMERA_CALL_VIDEO_START };
    for (size_t k = 0U; k < 3U; k++) {
        kode_value_t v = text(longest);
        asks(PATH_ROWS[k], &v, PATH_CALL[k]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(longest, g_camera.last.path, PATH_ROWS[k]);

        char too_long[CONSOLE_PATH_MAX + 2U];
        (void)memcpy(too_long, longest, CONSOLE_PATH_MAX);
        too_long[CONSOLE_PATH_MAX]      = 'b';   /* 97 characters */
        too_long[CONSOLE_PATH_MAX + 1U] = '\0';
        v = text(too_long);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(PATH_ROWS[k], &v), PATH_ROWS[k]);
        v = text("");
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(PATH_ROWS[k], &v), PATH_ROWS[k]);
        v = text(NULL);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(PATH_ROWS[k], &v), PATH_ROWS[k]);
        TEST_ASSERT_EQUAL_MESSAGE(KODE_DENIED, fake_console_run(PATH_ROWS[k], NULL), PATH_ROWS[k]);
        TEST_ASSERT_NULL_MESSAGE(fake_kode_take(), PATH_ROWS[k]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_kode_blocks_out(), PATH_ROWS[k]);
    }
    /* The console's own limit is longer than the row's: 257 characters never reach it either. */
    char huge[KODE_ARG_STR_MAX + 2U];
    (void)memset(huge, 'c', sizeof(huge));
    huge[KODE_ARG_STR_MAX + 1U] = '\0';
    kode_value_t v = text(huge);
    refused("cam_photo", &v);
    TEST_ASSERT_EQUAL_UINT(3U, g_camera.calls);
}

TEST_CASE("cam_focus makes one pass of three seconds; cam_focus_mode maps off, auto and manual", "[console]")
{
    console_reset();

    asks("cam_focus", NULL, CAMERA_CALL_FOCUS);
    TEST_ASSERT_EQUAL_UINT32(3000U, g_camera.last.ms);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_camera.last.me);

    static const camera_focus_mode_t MODE[3] = { CAMERA_FOCUS_OFF, CAMERA_FOCUS_AUTO, CAMERA_FOCUS_MANUAL };
    for (uint8_t k = 0U; k < 3U; k++) {
        kode_value_t v = word(k);
        asks("cam_focus_mode", &v, CAMERA_CALL_FOCUS_MODE);
        TEST_ASSERT_EQUAL_MESSAGE(MODE[k], g_camera.last.mode, FUNCTION_NAME[3]);
    }
    kode_value_t v = word(3U);
    refused("cam_focus_mode", &v);
    refused("cam_focus_mode", NULL);
}

TEST_CASE("cam_lens takes 0 to 1023 and hands the lens that number; one outside is refused", "[console]")
{
    console_reset();

    static const int32_t LENS[] = { 0, 1, 21, 600, 1022, 1023 };
    for (size_t k = 0U; k < sizeof(LENS) / sizeof(LENS[0]); k++) {
        kode_value_t v = integer(LENS[k]);
        asks("cam_lens", &v, CAMERA_CALL_FOCUS_AT);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)LENS[k], g_camera.last.lens);
    }
    kode_value_t v = integer(-1);
    refused("cam_lens", &v);
    v = integer(1024);
    refused("cam_lens", &v);
    v = integer(65536);   /* would be 0 in a uint16_t */
    refused("cam_lens", &v);
    refused("cam_lens", NULL);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.bad_args);
}

TEST_CASE("cam_standby is on or off, and says so to camera_sys_standby()", "[console]")
{
    console_reset();
    kode_value_t v = boolean(true);
    asks("cam_standby", &v, CAMERA_CALL_STANDBY);
    TEST_ASSERT_TRUE(g_camera.last.on);
    v = boolean(false);
    asks("cam_standby", &v, CAMERA_CALL_STANDBY);
    TEST_ASSERT_FALSE(g_camera.last.on);
    refused("cam_standby", NULL);
}

TEST_CASE("cam_balance maps its six words onto camera_sys.h's balances, manual left out", "[console]")
{
    console_reset();
    static const camera_white_balance_t WANT[6] = {
        CAMERA_WHITE_BALANCE_AUTO,   CAMERA_WHITE_BALANCE_SUNNY, CAMERA_WHITE_BALANCE_CLOUDY,
        CAMERA_WHITE_BALANCE_OFFICE, CAMERA_WHITE_BALANCE_HOME,  CAMERA_WHITE_BALANCE_NEUTRAL,
    };
    for (uint8_t k = 0U; k < 6U; k++) {
        kode_value_t v = word(k);
        asks("cam_balance", &v, CAMERA_CALL_SET);
        TEST_ASSERT_EQUAL_MESSAGE(CAMERA_WHITE_BALANCE, g_camera.last.setting, "cam_balance");
        TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)WANT[k], g_camera.last.value, "cam_balance");
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int32_t)CAMERA_WHITE_BALANCE_MANUAL, g_camera.last.value, "cam_balance");
    }
    kode_value_t v = word(6U);
    refused("cam_balance", &v);
    refused("cam_balance", NULL);

    /* Neutral is the sixth word, balance 6, not the sixth enum value. */
    v = word(5U);
    asks("cam_balance", &v, CAMERA_CALL_SET);
    TEST_ASSERT_EQUAL_INT32(6, g_camera.last.value);
}

/** @brief Row @p name, through every value from @p lo to @p hi, sets @p setting to it; one either side is refused. */
static void setting_row(const char *name, camera_setting_t setting, int32_t lo, int32_t hi)
{
    for (int32_t n = lo; n <= hi; n++) {
        kode_value_t v = integer(n);
        asks(name, &v, CAMERA_CALL_SET);
        TEST_ASSERT_EQUAL_MESSAGE(setting, g_camera.last.setting, name);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(n, g_camera.last.value, name);
    }
    kode_value_t v = integer(lo - 1);
    refused(name, &v);
    v = integer(hi + 1);
    refused(name, &v);
    refused(name, NULL);
}

TEST_CASE("cam_effect, brightness, contrast, saturation and exposure_ev set their own setting, inside their limits", "[console]")
{
    console_reset();
    setting_row("cam_effect", CAMERA_EFFECT, 0, 6);
    setting_row("cam_brightness", CAMERA_BRIGHTNESS, -3, 3);
    setting_row("cam_contrast", CAMERA_CONTRAST, -3, 3);
    setting_row("cam_saturation", CAMERA_SATURATION, -4, 4);
    setting_row("cam_exposure_ev", CAMERA_EXPOSURE_COMPENSATION, -3, 3);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.bad_args);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
}

TEST_CASE("the movie starts at the path without sound, and stops; cam_defaults resets the settings", "[console]")
{
    console_reset();

    kode_value_t v = text("/sdcard/m.mov");
    asks("cam_video_start", &v, CAMERA_CALL_VIDEO_START);
    TEST_ASSERT_EQUAL_STRING("/sdcard/m.mov", g_camera.last.path);
    TEST_ASSERT_TRUE(g_camera.last.has_movie);
    TEST_ASSERT_FALSE(g_camera.last.movie.audio);
    TEST_ASSERT_EQUAL_PTR(AO_ConsoleActions, g_camera.last.me);
    /* The rest is camera_sys.h's own: 800 x 600, its quality and rate, no limit. */
    TEST_ASSERT_EQUAL_UINT16(0U, g_camera.last.movie.width);
    TEST_ASSERT_EQUAL_UINT16(0U, g_camera.last.movie.height);
    TEST_ASSERT_EQUAL_UINT8(0U, g_camera.last.movie.fps);
    TEST_ASSERT_EQUAL_UINT32(0U, g_camera.last.movie.max_ms);

    asks("cam_video_stop", NULL, CAMERA_CALL_VIDEO_STOP);
    asks("cam_defaults", NULL, CAMERA_CALL_SETTINGS_RESET);
    TEST_ASSERT_EQUAL_UINT(3U, g_camera.calls);
}

/* ---- What comes of it ---------------------------------------------------- */

TEST_CASE("cam_last holds the request and the status the camera refused it with, for every status", "[console]")
{
    for (int rc = (int)KODE_OK; rc <= (int)KODE_IS_DIR; rc++) {
        console_reset();
        g_camera.answer = (kode_status_t)rc;
        TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
        serve();
        if (rc == (int)KODE_OK) {
            /* Taken: nothing is said until the answer. */
            TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
            last_is("none", 0);
            answer_all_ok();
            last_is("focus", KODE_OK);
            TEST_ASSERT_EQUAL_STRING("cam_focus: done", fake_kode_last_line());
        } else {
            last_is("focus", (kode_status_t)rc);
            TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
            logged_status("cam_focus", (kode_status_t)rc);
            TEST_ASSERT_FALSE(g_camera.door[DOOR_FOCUS]);   /* not taken: no answer to wait for */
        }
    }
}

TEST_CASE("cam_last holds the status of the answer a request came to, for every status, and its reason is on the log", "[console]")
{
    for (int rc = (int)KODE_OK; rc <= (int)KODE_IS_DIR; rc++) {
        console_reset();
        TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
        serve();
        TEST_ASSERT_TRUE(g_camera.door[DOOR_FOCUS]);
        answer_focus((kode_status_t)rc, (rc == (int)KODE_OK) ? NULL : "no focus pass could run", false);
        last_is("focus", (kode_status_t)rc);
        if (rc == (int)KODE_OK) {
            TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
            TEST_ASSERT_EQUAL_STRING("cam_focus: done", fake_kode_last_line());
            TEST_ASSERT_TRUE(fake_kode_logged("cam_focus: no focus found"));
        } else {
            TEST_ASSERT_EQUAL_UINT(2U, g_kode.warnings);   /* the refusal and its reason */
            TEST_ASSERT_EQUAL_STRING("cam_focus: no focus pass could run", fake_kode_last_line());
        }
    }
}

TEST_CASE("a pass that found focus says so; a refused setting or photo is on the log with its name and status", "[console]")
{
    console_reset();
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
    serve();
    answer_focus(KODE_OK, NULL, true);
    TEST_ASSERT_TRUE(fake_kode_logged("cam_focus: focused"));

    g_camera.answer = KODE_TIMEOUT;
    kode_value_t v = integer(2);
    asks("cam_brightness", &v, CAMERA_CALL_SET);   /* a command: refused at once */
    logged_status("a setting", KODE_TIMEOUT);
    last_is("set", KODE_TIMEOUT);

    v = text("/sdcard/a.jpg");
    asks("cam_photo", &v, CAMERA_CALL_PHOTO_SAVE);   /* a request not taken: refused at once, no door open */
    logged_status("cam_photo", KODE_TIMEOUT);
    last_is("photo", KODE_TIMEOUT);

    asks("cam_defaults", NULL, CAMERA_CALL_SETTINGS_RESET);
    logged_status("cam_defaults", KODE_TIMEOUT);
    asks("cam_video_stop", NULL, CAMERA_CALL_VIDEO_STOP);
    logged_status("cam_video_stop", KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT(4U, g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.errors);
}

TEST_CASE("a photo, a movie start and a movie stop come to what their answers say", "[console]")
{
    console_reset();

    kode_value_t v = text("/sdcard/a.jpg");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_photo", &v));
    serve();
    answer_save(KODE_DENIED, "the folder is not there");
    last_is("photo", KODE_DENIED);
    TEST_ASSERT_EQUAL_STRING("cam_photo: the folder is not there", fake_kode_last_line());

    v = text("/sdcard/m.mov");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_video_start", &v));
    serve();
    answer_video_start(KODE_NO_RESOURCES, NULL);
    last_is("video_start", KODE_NO_RESOURCES);
    logged_status("cam_video_start", KODE_NO_RESOURCES);

    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_video_stop", NULL));
    serve();
    answer_video_stop(KODE_OK, NULL);
    last_is("video_stop", KODE_OK);
    TEST_ASSERT_EQUAL_STRING("cam_video_stop: done", fake_kode_last_line());
}

TEST_CASE("cam_last is the last request that finished, not the one running", "[console]")
{
    console_reset();
    kode_value_t v = text("/sdcard/a.jpg");
    asks("cam_photo", &v, CAMERA_CALL_PHOTO_SAVE);
    last_is("photo", KODE_OK);

    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
    serve();
    last_is("photo", KODE_OK);   /* focus is taken, and not answered yet */
    answer_focus(KODE_OK, NULL, true);
    last_is("focus", KODE_OK);
}

TEST_CASE("two requests of one kind: the second finds the door open, is refused with a warning, and the first still comes", "[console]")
{
    console_reset();
    kode_value_t v = text("/sdcard/a.jpg");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_photo", &v));
    serve();
    TEST_ASSERT_TRUE(g_camera.door[DOOR_SAVE]);

    v = text("/sdcard/b.jpg");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_photo", &v));   /* still >ok: the row only posts */
    serve();
    TEST_ASSERT_EQUAL_UINT(1U, g_camera.busy);
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
    logged_status("cam_photo", KODE_DENIED);
    last_is("photo", KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());   /* its block went back */

    answer_save(KODE_OK, NULL);   /* the first one's answer */
    TEST_ASSERT_EQUAL_STRING("cam_photo: done, /sdcard/a.jpg", fake_kode_last_line());
    last_is("photo", KODE_OK);

    /* And the door is open for the next. */
    v = text("/sdcard/c.jpg");
    asks("cam_photo", &v, CAMERA_CALL_PHOTO_SAVE);
    TEST_ASSERT_EQUAL_STRING("/sdcard/c.jpg", g_camera.last.path);
}

TEST_CASE("requests of different kinds are open together: a photo, a focus pass and the movie", "[console]")
{
    console_reset();
    kode_value_t v = text("/sdcard/a.jpg");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_photo", &v));
    serve();
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
    serve();
    v = text("/sdcard/m.mov");
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_video_start", &v));
    serve();
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_video_stop", NULL));
    serve();
    TEST_ASSERT_TRUE(g_camera.door[DOOR_SAVE] && g_camera.door[DOOR_FOCUS] && g_camera.door[DOOR_VIDEO_START] &&
                     g_camera.door[DOOR_VIDEO_STOP]);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.busy);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);

    answer_video_stop(KODE_OK, NULL);   /* any order */
    answer_save(KODE_OK, NULL);
    answer_video_start(KODE_OK, NULL);
    answer_focus(KODE_OK, NULL, true);
    TEST_ASSERT_EQUAL_UINT(0U, g_kode.warnings);
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
}

TEST_CASE("the camera refusing a request leaves the AO free and says so on the log; the next goes through", "[console]")
{
    console_reset();
    g_camera.answer = KODE_NO_RESOURCES;
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_focus", NULL));
    serve();
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
    logged_status("cam_focus", KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(g_camera.door[DOOR_FOCUS]);

    g_camera.answer = KODE_OK;
    asks("cam_focus", NULL, CAMERA_CALL_FOCUS);
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
}

TEST_CASE("the news it did not get is said on the log; the rows are on the console anyway", "[console]")
{
    fakes_reset();
    g_camera.subscribe_answer = KODE_DENIED;
    console_up();
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.errors);
    TEST_ASSERT_TRUE(fake_kode_logged("the camera's news are refused"));
    TEST_ASSERT_EQUAL_UINT(FUNCTION_COUNT, fake_console_functions());
    TEST_ASSERT_EQUAL_UINT(VARIABLE_COUNT, fake_console_variables());
    asks("cam_focus", NULL, CAMERA_CALL_FOCUS);   /* and it works without them */
}

TEST_CASE("the camera's news are on the log and never change cam_last", "[console]")
{
    console_reset();
    news(CAMERA_MODULE_FOUND_SIG, 1);
    TEST_ASSERT_EQUAL_STRING("the camera is here (upright)", fake_kode_last_line());
    news(CAMERA_MODULE_FOUND_SIG, 2);
    TEST_ASSERT_EQUAL_STRING("the camera is here (turned round)", fake_kode_last_line());
    news(CAMERA_MODULE_LOST_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("the camera is gone", fake_kode_last_line());
    news(CAMERA_WRONG_MODULE_SIG, (int32_t)CAMERA_WRONG_SENSOR);
    TEST_ASSERT_EQUAL_STRING("that is not this camera (wrong sensor)", fake_kode_last_line());
    news(CAMERA_VIDEO_ENDED_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("the movie ended (0)", fake_kode_last_line());
    news(CAMERA_MEMORY_EXHAUSTED_SIG, 0);
    TEST_ASSERT_EQUAL_STRING("no memory for the frames: the camera gave up", fake_kode_last_line());
    /* The ones the example does not follow go up to the parent, as the state says. */
    camera_sys_news_t other = { .super = { .sig = (QSignal)CAMERA_FOCUS_DONE_SIG }, .value = 1 };
    TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &other.super));
    last_is("none", 0);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);   /* news make no call */
}

TEST_CASE("with no event block or queue slot to spare, the row answers no resources and leaves nothing out", "[console]")
{
    console_reset();
    kode_value_t v = text("/sdcard/a.jpg");

    /* One block is kept for the rest of the app. */
    g_kode.blocks_held = FAKE_KODE_BLOCKS - 1U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("cam_photo", &v));
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
    g_kode.blocks_held = FAKE_KODE_BLOCKS - 2U;
    TEST_ASSERT_EQUAL(KODE_OK, fake_console_run("cam_photo", &v));
    serve();
    answer_all_ok();
    g_kode.blocks_held = 0U;

    /* One slot of the queue likewise: the post is refused and hands its block back. */
    g_kode.queue_held = g_kode.queue_len - 1U;
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, fake_console_run("cam_focus", NULL));
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
    TEST_ASSERT_NULL(fake_kode_take());
    g_kode.queue_held = g_kode.queue_len - 2U;
    asks("cam_focus", NULL, CAMERA_CALL_FOCUS);
}

TEST_CASE("an event the AO does not know goes up to its parent; a row kind it does not know is refused", "[console]")
{
    console_reset();
    QEvt const stray = { .sig = (QSignal)Q_ENTRY_SIG };
    TEST_ASSERT_EQUAL_UINT8(Q_SUPER(&QHsm_top), ConsoleActions_serving(&l_ao, &stray));
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);

    RequestEvt odd = { .super = { .sig = (QSignal)CC_REQUEST_SIG }, .kind = (request_kind_t)99 };
    apply(&l_ao, &odd);
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
    TEST_ASSERT_EQUAL_UINT(1U, g_kode.warnings);
}

/* ---- Variables ----------------------------------------------------------- */

TEST_CASE("cam_state is the state's name as one word, for every state", "[console]")
{
    console_reset();
    static const struct {
        camera_state_t state;
        const char    *word;
    } STATE[] = {
        { CAMERA_OFF, "off" },
        { CAMERA_HEADER_BUSY, "header_busy" },
        { CAMERA_LOOKING, "looking" },
        { CAMERA_NO_SENSOR, "no_sensor" },
        { CAMERA_WRONG_SENSOR, "wrong_sensor" },
        { CAMERA_STARTING, "starting" },
        { CAMERA_RUNNING, "running" },
        { CAMERA_RESTARTING, "restarting" },
        { CAMERA_STANDBY, "standby" },
        { CAMERA_NO_MEMORY, "no_memory" },
        { (camera_state_t)99, "?" },
    };
    for (size_t k = 0U; k < sizeof(STATE) / sizeof(STATE[0]); k++) {
        g_camera.st.state = STATE[k].state;
        reads("cam_state", STATE[k].word);
        all_variables_read();
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
}

TEST_CASE("cam_fps is tenths with a point; the counters are plain numbers, to the largest", "[console]")
{
    console_reset();
    static const struct {
        uint16_t    x10;
        const char *word;
    } FPS[] = {
        { 0U, "0.0" }, { 9U, "0.9" }, { 70U, "7.0" }, { 143U, "14.3" }, { 300U, "30.0" }, { 65535U, "6553.5" },
    };
    for (size_t k = 0U; k < sizeof(FPS) / sizeof(FPS[0]); k++) {
        g_camera.st.fps_x10 = FPS[k].x10;
        reads("cam_fps", FPS[k].word);
    }
    g_camera.st.frames   = 0U;
    g_camera.st.restarts = 0U;
    reads("cam_frames", "0");
    reads("cam_restarts", "0");
    g_camera.st.frames   = 123456U;
    g_camera.st.restarts = 7U;
    reads("cam_frames", "123456");
    reads("cam_restarts", "7");
    g_camera.st.frames   = UINT32_MAX;
    g_camera.st.restarts = UINT32_MAX;
    reads("cam_frames", "4294967295");
    reads("cam_restarts", "4294967295");
}

TEST_CASE("cam_rotated and cam_big_now are on or off", "[console]")
{
    console_reset();
    reads("cam_rotated", "off");
    reads("cam_big_now", "off");
    g_camera.st.rotated = true;
    reads("cam_rotated", "on");
    reads("cam_big_now", "off");
    g_camera.st.big_photo = true;
    reads("cam_rotated", "on");
    reads("cam_big_now", "on");
    g_camera.st.rotated = false;
    reads("cam_rotated", "off");
    reads("cam_big_now", "on");
}

TEST_CASE("cam_lens_now is the lens's number; cam_focus_now is one word for each focus state", "[console]")
{
    console_reset();
    static const uint16_t LENS[] = { 0U, CAMERA_LENS_REST, 600U, CAMERA_LENS_NEAR, 65535U };
    static const char *const LENS_WORD[] = { "0", "21", "600", "1023", "65535" };
    for (size_t k = 0U; k < sizeof(LENS) / sizeof(LENS[0]); k++) {
        g_camera.focus.lens = LENS[k];
        reads("cam_lens_now", LENS_WORD[k]);
    }

    static const struct {
        camera_focus_state_t state;
        bool                 focused;
        const char          *word;
    } FOCUS[] = {
        { CAMERA_FOCUS_IDLE, false, "idle" },
        { CAMERA_FOCUS_IDLE, true, "focused" },
        { CAMERA_FOCUS_LOADING, false, "loading" },
        { CAMERA_FOCUS_LOADING, true, "loading" },
        { CAMERA_FOCUS_FOCUSING, false, "focusing" },
        { CAMERA_FOCUS_FOCUSING, true, "focusing" },
        { CAMERA_FOCUS_MOVING, false, "moving" },
        { CAMERA_FOCUS_MOVING, true, "moving" },
        { CAMERA_FOCUS_FAILED, false, "failed" },
        { CAMERA_FOCUS_FAILED, true, "failed" },
        { (camera_focus_state_t)99, false, "idle" },
    };
    for (size_t k = 0U; k < sizeof(FOCUS) / sizeof(FOCUS[0]); k++) {
        g_camera.focus.state   = FOCUS[k].state;
        g_camera.focus.focused = FOCUS[k].focused;
        reads("cam_focus_now", FOCUS[k].word);
    }
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
}

TEST_CASE("cam_video is idle, or rec_ and the whole seconds recorded", "[console]")
{
    console_reset();
    reads("cam_video", "idle");

    g_camera.video.recording = true;
    static const struct {
        uint32_t    ms;
        const char *word;
    } REC[] = {
        { 0U, "rec_0s" }, { 999U, "rec_0s" }, { 1000U, "rec_1s" }, { 12345U, "rec_12s" },
        { 3599000U, "rec_3599s" }, { UINT32_MAX, "rec_4294967s" },
    };
    for (size_t k = 0U; k < sizeof(REC) / sizeof(REC[0]); k++) {
        g_camera.video.duration_ms = REC[k].ms;
        reads("cam_video", REC[k].word);
    }
    g_camera.video.recording = false;   /* stopped: the time it ran is not shown */
    reads("cam_video", "idle");
}

TEST_CASE("cam_last starts as none:0", "[console]")
{
    console_reset();
    reads("cam_last", "none:0");
}

/** @brief The ten variables' words, in VARIABLE_NAME's order, for one state of the camera. */
typedef struct {
    const char *what;
    const char *word[VARIABLE_COUNT];
} scene_t;

static void scene_set(size_t k)
{
    fake_camera_reset();
    switch (k) {
        case 0U:   /* no camera on J3 */
            g_camera.st.state = CAMERA_LOOKING;
            g_camera.focus.mode = CAMERA_FOCUS_OFF;
            g_camera.focus.lens = 0U;
            break;
        case 1U:   /* running, in focus */
            g_camera.st.fps_x10 = 143U;
            g_camera.st.frames = 1234U;
            g_camera.st.restarts = 2U;
            g_camera.focus.lens = 600U;
            g_camera.focus.focused = true;
            break;
        case 2U:   /* plugged in turned round */
            g_camera.st.fps_x10 = 143U;
            g_camera.st.frames = 99U;
            g_camera.st.rotated = true;
            break;
        case 3U:   /* recording */
            g_camera.st.fps_x10 = 143U;
            g_camera.st.frames = 5000U;
            g_camera.video.recording = true;
            g_camera.video.duration_ms = 12345U;
            break;
        default:   /* a big photo: the preview is out, the state stays running */
            g_camera.st.big_photo = true;
            g_camera.st.frames = 5000U;
            g_camera.focus.state = CAMERA_FOCUS_FOCUSING;
            break;
    }
}

TEST_CASE("every variable is the word the console sends in each state: no camera, running, rotated, recording, big photo", "[console]")
{
    console_reset();
    static const scene_t SCENE[5] = {
        { "no camera", { "looking", "0.0", "0", "0", "off", "0", "idle", "idle", "off", "none:0" } },
        { "running",   { "running", "14.3", "1234", "2", "off", "600", "focused", "idle", "off", "none:0" } },
        { "rotated",   { "running", "14.3", "99", "0", "on", "21", "idle", "idle", "off", "none:0" } },
        { "recording", { "running", "14.3", "5000", "0", "off", "21", "idle", "rec_12s", "off", "none:0" } },
        { "big photo", { "running", "0.0", "5000", "0", "off", "21", "focusing", "idle", "on", "none:0" } },
    };
    for (size_t s = 0U; s < 5U; s++) {
        scene_set(s);
        unsigned const reads_before = g_camera.reads;
        for (size_t k = 0U; k < VARIABLE_COUNT; k++) {
            char msg[64];
            (void)snprintf(msg, sizeof(msg), "%s / %s", SCENE[s].what, VARIABLE_NAME[k]);
            char out[KODE_VALUE_STR_MAX + 1U];
            TEST_ASSERT_EQUAL_MESSAGE(KODE_OK, fake_console_read(VARIABLE_NAME[k], out), msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(SCENE[s].word[k], out, msg);
        }
        TEST_ASSERT_TRUE(g_camera.reads > reads_before);
        TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);   /* a variable asks nothing of the camera: it reads a copy */
    }
}

TEST_CASE("reading a variable is no request: nothing is posted, run, or changed", "[console]")
{
    console_reset();
    scene_set(3U);
    for (unsigned round = 0U; round < 3U; round++) {
        all_variables_read();
    }
    TEST_ASSERT_NULL(fake_kode_take());
    TEST_ASSERT_EQUAL_UINT(0U, g_camera.calls);
    TEST_ASSERT_EQUAL_UINT(0U, fake_kode_blocks_out());
    reads("cam_last", "none:0");
}

TEST_CASE("a variable that cannot be carried is not hidden: the longest words are within the console's 32 characters", "[console]")
{
    console_reset();
    g_camera.st.state          = CAMERA_WRONG_SENSOR;
    g_camera.st.fps_x10        = UINT16_MAX;
    g_camera.st.frames         = UINT32_MAX;
    g_camera.st.restarts       = UINT32_MAX;
    g_camera.focus.lens        = UINT16_MAX;
    g_camera.focus.state       = CAMERA_FOCUS_FOCUSING;
    g_camera.video.recording   = true;
    g_camera.video.duration_ms = UINT32_MAX;
    all_variables_read();
    last_is("none", 0);
}
