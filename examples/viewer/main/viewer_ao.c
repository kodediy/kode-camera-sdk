/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file viewer_ao.c
 * @brief The camera on the glass, from an Active Object of your own, with camera_sys.h: zoom,
 *        mirror, and focus where you touch.
 *
 * What it teaches: the calls an app needs to show the camera and to use it.
 * On every render pass the AO asks for the newest frame without waiting
 * (camera_sys_frame_take(): it never waits), fits it upright into a
 * picture the size of the glass (camera_sys_fit_with_options(): the middle of the frame, no
 * bands, with the zoom and the mirror) and gives it back. The picture is an
 * LVGL canvas; everything else - LVGL, the flush, the touch - is kode-sdk's
 * kscreen, as in its display_lvgl example, whose comments say why it is done
 * this way.
 *
 * Touch the picture and the lens focuses there: the touch is a point of the
 * picture that was drawn, camera_sys_fit_point() says which point of the frame it is
 * (with the zoom and the mirror in the way), and camera_sys_focus_point() asks for a
 * focus pass on it. Being a request of this layer it does not wait: it returns, and
 * CAMERA_FOCUS_RSP_SIG says how the pass went (a second request while one is out is
 * refused, so the AO asks one at a time). A ring shows the pass - white while it
 * searches, green when it found the focus, red when the scene is too flat to say - and
 * goes out a moment after. Nothing is asked of the camera until a finger touches the glass.
 *
 * Try it: plug the Camera module into J3, either way up, before or after
 * starting. Until frames arrive the glass says what camera_sys_status_read() says -
 * looking, no sensor, header busy - and then shows the picture, the right
 * way up both ways round. The pad: up and down zoom, left mirrors, right puts
 * everything back (and asks for the focus in the middle). The bottom button goes home.
 *
 * The frames arrive at 14 a second and the render pass runs faster, so most
 * passes find nothing new and leave the canvas alone.
 */
#include "viewer_ao.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "camera_sys.h"
#include "kode_log.h"
#include "kscreen.h"

#define TAG "viewer"

/** @brief The zoom steps, in sixteenths: 1x, 1.5x, 2x, 3x, 4x, 6x, 8x. */
static const uint16_t ZOOMS[] = { 16U, 24U, 32U, 48U, 64U, 96U, 128U };
#define ZOOM_STEPS ((int)(sizeof(ZOOMS) / sizeof(ZOOMS[0])))

#define RING_SIZE      72     /**< The focus ring's diameter, pixels.            */
#define RING_HOLD_MS   1200U  /**< How long it stays after the pass has ended.   */
#define HUD_EVERY_MS   500U

/** @brief This AO's private signals. */
enum ViewerSignals {
    VW_TICK_SIG = MAX_SIG   /**< The render pass: kscreen's one-shot. */
};

typedef struct {
    QActive        super;
    kscreen_t      screen;
    lv_obj_t      *canvas;    /**< The picture: the whole glass.                 */
    lv_obj_t      *note;      /**< What camera_sys_status_read() says while there is no frame. */
    lv_obj_t      *hud;       /**< Frame rate, zoom and lens, top left.          */
    lv_obj_t      *ring;      /**< Where the last touch asked for focus.         */
    uint8_t       *px;        /**< The canvas's pixels, in PSRAM.                */
    size_t         px_bytes;  /**< What was allocated: camera_fit_with_options() is told.  */
    bool           focus_busy; /**< A focus request is out (its answer is not in): one at a time. */
    uint16_t       w;
    uint16_t       h;
    camera_state_t shown;     /**< The state the note says, to write it once.    */
    camera_fit_options_t fit;       /**< How the frame is drawn: the touch needs it.   */
    int            zoom;      /**< Index in ZOOMS.                               */
    bool           down;      /**< The finger is on the glass (a tap is its first sample). */
    bool           ring_on;
    uint32_t       ring_end;  /**< When the pass ended, 0 while it runs.         */
    uint32_t       hud_t;
} Viewer;

/* A slot for each door of the camera (the answers come without margin), and the screen's and the
 * buttons' own events. The news are not subscribed to. */
static QEvtPtr     l_queue[CAMERA_SYS_DOORS + KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Viewer l_ao;
QActive * const AO_Viewer = &l_ao.super;

static QState Viewer_initial(Viewer * const me, void const * const par);
static QState Viewer_running(Viewer * const me, QEvt const * const e);
static QState Viewer_waiting(Viewer * const me, QEvt const * const e);
static QState Viewer_drawing(Viewer * const me, QEvt const * const e);

/* ---- What is on the glass ------------------------------------------------ */

/** @brief The canvas, black, the note, the ring and the numbers over it. */
static void ui_build(Viewer * const me)
{
    lv_obj_t * const scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    me->w = (uint16_t)lv_display_get_horizontal_resolution(me->screen.disp);
    me->h = (uint16_t)lv_display_get_vertical_resolution(me->screen.disp);
    size_t const bytes = (size_t)me->w * me->h * 2U;
    me->px = heap_caps_aligned_alloc(64U, bytes, MALLOC_CAP_SPIRAM);
    me->px_bytes = (me->px != NULL) ? bytes : 0U;
    if ( me->px != NULL )
    {
        memset(me->px, 0, bytes);
        me->canvas = lv_canvas_create(scr);
        lv_canvas_set_buffer(me->canvas, me->px, me->w, me->h, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(me->canvas);
    }
    else
    {
        KLOG_E(TAG, "no PSRAM for a %ux%u picture", (unsigned)me->w, (unsigned)me->h);
    }

    /* The picture is the whole glass, so a touch at (x, y) is that pixel of it. */
    me->fit.width  = me->w;
    me->fit.height = me->h;
    me->fit.fill   = CAMERA_COVER;

    me->ring = lv_obj_create(scr);
    lv_obj_remove_style_all(me->ring);
    lv_obj_set_size(me->ring, RING_SIZE, RING_SIZE);
    lv_obj_set_style_radius(me->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(me->ring, 3, 0);
    lv_obj_set_style_border_color(me->ring, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(me->ring, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(me->ring, false);
    lv_obj_set_hidden(me->ring, true);

    me->hud = lv_label_create(scr);
    lv_obj_set_style_text_color(me->hud, lv_color_white(), 0);
    lv_obj_set_style_bg_color(me->hud, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(me->hud, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(me->hud, 4, 0);
    lv_obj_align(me->hud, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_text(me->hud, "");

    me->note = lv_label_create(scr);
    lv_obj_set_style_text_color(me->note, lv_color_white(), 0);
    lv_obj_center(me->note);
    me->shown = CAMERA_RUNNING;   /* anything but the first state shown */
}

/** @brief One render pass's worth: the newest frame, or why there is none. */
static void frame_show(Viewer * const me)
{
    camera_frame_t f;

    if ( (me->px != NULL) && (camera_sys_frame_take(&f) == KODE_OK) )
    {
        (void)camera_sys_fit_with_options(&f, me->px, me->px_bytes, &me->fit);
        camera_sys_frame_give(&f);
        /* The same buffer again: LVGL drops what it cached and redraws. */
        lv_canvas_set_buffer(me->canvas, me->px, me->w, me->h, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_hidden(me->note, true);
        return;
    }
    camera_status_t i;
    if ( (camera_sys_status_read(&i) == KODE_OK) && (i.state != CAMERA_RUNNING) && (i.state != me->shown) )
    {
        me->shown = i.state;
        lv_label_set_text_fmt(me->note, "Camera: %s", camera_sys_state_name(i.state));
        lv_obj_set_hidden(me->note, false);
        KLOG_I(TAG, "camera: %s", camera_sys_state_name(i.state));
    }
}

/** @brief The ring follows the pass: white searching, then green or red, then gone. */
static void ring_update(Viewer * const me)
{
    if ( !me->ring_on )
    {
        return;
    }
    camera_focus_info_t f;
    uint32_t const now = kode_sys_time_ms();

    camera_sys_focus_info(&f);
    if ( f.searching )
    {
        me->ring_end = 0U;
        lv_obj_set_style_border_color(me->ring, lv_color_white(), 0);
        return;
    }
    if ( me->ring_end == 0U )
    {
        me->ring_end = now;
        KLOG_I(TAG, "%s: lens %u, %u ms", f.focused ? "in focus" : "no focus found",
               (unsigned)f.lens, (unsigned)f.pass_ms);
    }
    lv_obj_set_style_border_color(me->ring, f.focused ? lv_color_make(0x30, 0xD1, 0x58)
                                                      : lv_color_make(0xFF, 0x45, 0x3A), 0);
    if ( (now - me->ring_end) >= RING_HOLD_MS )
    {
        me->ring_on = false;
        lv_obj_set_hidden(me->ring, true);
    }
}

/** @brief The numbers, twice a second. */
static void hud_update(Viewer * const me)
{
    uint32_t const now = kode_sys_time_ms();

    if ( (now - me->hud_t) < HUD_EVERY_MS )
    {
        return;
    }
    me->hud_t = now;
    camera_status_t i;
    camera_focus_info_t f;
    camera_sys_focus_info(&f);
    if ( camera_sys_status_read(&i) != KODE_OK )
    {
        return;
    }
    uint32_t const z = ZOOMS[me->zoom];
    lv_label_set_text_fmt(me->hud, "%u.%u fps  %u.%ux%s  lens %u", (unsigned)(i.fps_x10 / 10U),
                          (unsigned)(i.fps_x10 % 10U), (unsigned)(z / 16U), (unsigned)((z % 16U) * 10U / 16U),
                          me->fit.mirror ? "  mirror" : "", (unsigned)f.lens);
}

/* ---- Touch and buttons ----------------------------------------------------- */

/**
 * @brief Ask for a focus pass on a point of the frame.
 * @return Whether it was asked: false while the last one is still out, or when the camera refuses.
 *         The answer is CAMERA_FOCUS_RSP_SIG.
 */
static bool focus_ask(Viewer * const me, uint16_t x, uint16_t y)
{
    if ( me->focus_busy )
    {
        return false;
    }
    if ( camera_sys_focus_point(&me->super, x, y, 0U) != KODE_OK )
    {
        return false;
    }
    me->focus_busy = true;
    return true;
}

/** @brief A touch: its first sample asks for focus where it landed. */
static void touch_focus(Viewer * const me, TouchEvt const * const t)
{
    bool const first = t->pressed && !me->down;

    me->down = t->pressed;
    if ( !first || (t->x < 0) || (t->y < 0) )
    {
        return;
    }
    uint16_t fx = 0U;
    uint16_t fy = 0U;
    if ( camera_sys_fit_point(&me->fit, (uint16_t)t->x, (uint16_t)t->y, &fx, &fy) != KODE_OK )
    {
        return;   /* outside the picture */
    }
    /* The request does not wait: its answer, CAMERA_FOCUS_RSP_SIG, frees the next one. */
    bool const asked = focus_ask(me, fx, fy);

    KLOG_I(TAG, "touch (%d,%d) is the frame's (%u,%u): %s", (int)t->x, (int)t->y, (unsigned)fx,
           (unsigned)fy, asked ? "focusing" : "not asked: the last request is still out");
    if ( asked )
    {
        me->ring_on  = true;
        me->ring_end = 0U;
        lv_obj_set_pos(me->ring, (int32_t)t->x - (RING_SIZE / 2), (int32_t)t->y - (RING_SIZE / 2));
        lv_obj_set_style_border_color(me->ring, lv_color_white(), 0);
        lv_obj_set_hidden(me->ring, false);
    }
}

/** @brief The pad. @return Whether it was the pad's. */
static bool pad_button(Viewer * const me, uint8_t id)
{
    switch ( (uint32_t)id )
    {
        case KODE_SYS_BUTTON_UP:
            if ( (me->zoom + 1) < ZOOM_STEPS )
            {
                me->zoom++;
            }
            break;
        case KODE_SYS_BUTTON_DOWN:
            if ( me->zoom > 0 )
            {
                me->zoom--;
            }
            break;
        case KODE_SYS_BUTTON_LEFT:
            me->fit.mirror = !me->fit.mirror;
            break;
        case KODE_SYS_BUTTON_RIGHT:
            me->zoom       = 0;
            me->fit.mirror = false;
            me->fit.pan_x  = 0;
            me->fit.pan_y  = 0;
            (void)focus_ask(me, CAMERA_FOCUS_CENTRE, CAMERA_FOCUS_CENTRE);
            break;
        default:
            return false;
    }
    me->fit.zoom_x16 = ZOOMS[me->zoom];
    return true;
}

/* ---- The AO -------------------------------------------------------------- */

void Viewer_ctor(void)
{
    Viewer * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&Viewer_initial));
    kscreen_ctor(&me->screen, &me->super, (enum_t)VW_TICK_SIG);
    me->canvas = NULL;
    me->note   = NULL;
    me->hud    = NULL;
    me->ring   = NULL;
    me->px     = NULL;
    me->px_bytes = 0U;
    me->focus_busy = false;
    memset(&me->fit, 0, sizeof(me->fit));
    me->zoom     = 0;
    me->down     = false;
    me->ring_on  = false;
    me->ring_end = 0U;
    me->hud_t    = 0U;
}

void Viewer_start(void)
{
    QActive_setAttr(AO_Viewer, TASK_NAME_ATTR, "viewer");
    QACTIVE_START(AO_Viewer, KODE_PRIO_USER_GUI,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

static QState Viewer_initial(Viewer * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    kscreen_start(&me->screen);
    return Q_TRAN(&Viewer_waiting);
}

/** @brief Whether or not there is a picture: the bottom button goes home. */
static QState Viewer_running(Viewer * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case CAMERA_FOCUS_RSP_SIG: {
            camera_sys_focus_answer_t const * const a = Q_EVT_CAST(camera_sys_focus_answer_t);

            me->focus_busy = false;
            if ( a->rc != KODE_OK )
            {
                KLOG_W(TAG, "the focus pass was refused: %s (%s)", camera_sys_status_text(a->rc),
                       (a->why != NULL) ? a->why : "no reason given");
                me->ring_on = false;
                if ( me->ring != NULL )
                {
                    lv_obj_set_hidden(me->ring, true);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                KLOG_I(TAG, "going home");
                if ( kode_sys_exit_to_os() != KODE_OK )
                {
                    KLOG_W(TAG, "AO_Boot is busy; press again");
                }
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/** @brief Waiting for AO_Display's answer: LVGL is not started yet. */
static QState Viewer_waiting(Viewer * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case DISPLAY_STATE_SIG: {
            if ( kscreen_open(&me->screen, (DisplayStateEvt const *)e) == KODE_OK )
            {
                status = Q_TRAN(&Viewer_drawing);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&Viewer_running);
            break;
        }
    }
    return status;
}

/** @brief Drawing: a frame, if there is a new one, on every render pass. */
static QState Viewer_drawing(Viewer * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            ui_build(me);
            status = Q_HANDLED();
            break;
        }
        case VW_TICK_SIG: {
            frame_show(me);
            ring_update(me);
            hud_update(me);
            kscreen_tick(&me->screen);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_EVT_SIG: {
            TouchEvt const * const t = (TouchEvt const *)e;

            kscreen_touch(&me->screen, t);
            touch_focus(me, t);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed && pad_button(me, be->id) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&Viewer_running);   /* the bottom button goes home */
            }
            break;
        }
        case DISPLAY_REPAINT_SIG: {
            kscreen_repaint(&me->screen);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Viewer_running);
            break;
        }
    }
    return status;
}
