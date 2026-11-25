/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_camera.h
 * @brief camera_sys.h as examples/console_actions reaches it: each call it makes written down, refused as the
 *        layer refuses an argument or a door that is open, and answered with what the case set.
 *
 * camera_sys_status_read(), camera_sys_focus_info(), camera_sys_video_info() and camera_sys_state_name(), the
 * immediates, answer from g_camera. The commands (a setting, the focus mode, the lens, the standby, the reset)
 * are done by their return value alone. The requests (a photo, a focus pass, the movie) are taken or refused by
 * their return value, and answered later, by the case: it builds the answer event and gives it to the AO, as
 * the owner would post it, and fake_camera_door_close() opens the door again just before, as the layer does.
 */
#ifndef FAKE_CAMERA_H_
#define FAKE_CAMERA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_sys.h"

/** @brief camera_sys.h's calls the example's rows ask for. */
typedef enum {
    CAMERA_CALL_NONE = 0,
    CAMERA_CALL_PHOTO_SAVE,
    CAMERA_CALL_FOCUS,
    CAMERA_CALL_FOCUS_MODE,
    CAMERA_CALL_FOCUS_AT,
    CAMERA_CALL_STANDBY,
    CAMERA_CALL_SET,
    CAMERA_CALL_VIDEO_START,
    CAMERA_CALL_VIDEO_STOP,
    CAMERA_CALL_SETTINGS_RESET
} camera_call_t;

/** @brief The doors the example can have open: one for each kind of request it makes. */
typedef enum { DOOR_SAVE, DOOR_FOCUS, DOOR_VIDEO_START, DOOR_VIDEO_STOP, DOOR_COUNT } fake_door_t;

#define FAKE_CAMERA_PATH 160U
#define PATH_LIMIT       122U   /**< camera_sys.h: "at most CAMERA_PATH_MAX - 1" characters. */

typedef struct {
    /* Set by the case; fake_camera_reset() leaves every answer KODE_OK and the camera running. */
    camera_status_t     st;      /**< What camera_sys_status_read() says.          */
    camera_focus_info_t focus;   /**< What camera_sys_focus_info() says.           */
    camera_video_info_t video;   /**< What camera_sys_video_info() says.           */
    kode_status_t       answer;  /**< The return of a call whose arguments are good and whose door is shut. */
    kode_status_t       subscribe_answer;   /**< camera_sys_subscribe()'s.         */

    /* Written down. */
    unsigned            calls;     /**< Commands and requests, refused ones too.   */
    unsigned            requests;  /**< ... of them, the requests.                 */
    unsigned            bad_args;  /**< ... refused for an argument the layer refuses. */
    unsigned            busy;      /**< ... refused for a door that was open.      */
    unsigned            reads;     /**< camera_sys_status_read()/focus_info()/video_info() calls. */
    unsigned            subscribed;/**< camera_sys_subscribe() calls.              */
    QActive            *subscriber;
    bool                door[DOOR_COUNT];   /**< Open: a request was taken and its answer is not given yet. */
    struct {
        camera_call_t          call;
        QActive               *me;                     /**< The asker of a request. */
        char                   path[FAKE_CAMERA_PATH]; /**< Photos and the movie.  */
        const char            *path_given;             /**< The pointer the call got. */
        camera_photo_options_t shot;                   /**< ... the options.       */
        bool                   has_shot;
        camera_video_options_t movie;
        bool                   has_movie;
        uint32_t               ms;                     /**< camera_sys_focus().    */
        camera_focus_mode_t    mode;
        uint16_t               lens;
        bool                   on;                     /**< camera_sys_standby().  */
        camera_setting_t       setting;
        int32_t                value;
    } last;                        /**< The call made last.                        */
} fake_camera_t;

extern fake_camera_t g_camera;

/** @brief Every record cleared, every answer KODE_OK, a camera running and in focus, every door shut. */
void fake_camera_reset(void);

/** @brief Shut @p door: the answer is about to be posted, and the layer opens it first. */
void fake_camera_door_close(fake_door_t door);

#endif /* FAKE_CAMERA_H_ */
