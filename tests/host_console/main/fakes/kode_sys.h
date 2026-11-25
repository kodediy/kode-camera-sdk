/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys.h
 * @brief kode-sdk's header for an Active Object (kode_sys.h) as
 *        examples/console_actions sees it on the host: the QP/C it uses, the
 *        console's table types and their two calls, and the log, each piece
 *        writing down what it was asked in fake_kode.c.
 *
 * Nothing here runs an Active Object. QACTIVE_START() calls the case's
 * g_kode.initial where QP/C would take the initial transition; Q_NEW_X()
 * hands out blocks of a small pool and QACTIVE_POST_X() queues them, both
 * keeping the margins QP/C keeps, and a refused post hands its block back as
 * QP/C does; the case dispatches what was posted to the state handler itself
 * (test_console.c #includes the example). The real camera_sys.h is included
 * over it. The example's target build, against the real kode_sys.h, is what
 * proves the rest: the types are kode-sdk's, field for field.
 */
#ifndef KODE_SYS_H_
#define KODE_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_log.h"

typedef int      enum_t;
typedef uint16_t QSignal;
typedef uint8_t  QState;
typedef uint32_t StackType_t;

/* QP/C casts every state handler to its own handler type; void (*)(void)
 * keeps the compiler's function-type check out of a cast it would make. The
 * case never calls through it: it calls the handlers by name. */
typedef void (*QStateHandler)(void);

typedef struct {
    QSignal sig;
} QEvt;

typedef QEvt const *QEvtPtr;

typedef struct {
    QStateHandler state;
} QActive;

enum { Q_ENTRY_SIG = 1, Q_EXIT_SIG, Q_INIT_SIG, MAX_SIG = 64 };

#define Q_STATE_CAST(handler_) ((QStateHandler)(handler_))
#define Q_HANDLED()            ((QState)0U)
#define Q_TRAN(target_)        ((void)(target_), (QState)1U)
#define Q_SUPER(super_)        ((void)(super_), (QState)2U)
#define Q_DIM(array_)          (sizeof(array_) / sizeof((array_)[0]))
#define Q_EVT_CAST(class_)     ((class_ const *)(e))

/* kqp_config.h's, at the pinned release. */
#define KQP_AO_QUEUE_LEN      16U
#define KQP_AO_STACK_BYTES    8192U
#define KQP_TICKS_PER_MS(ms_) (ms_)
#define KODE_PRIO_USER_LOW    1U
#define TASK_NAME_ATTR        0U

QState QHsm_top(void * const me, QEvt const * const e);

void QActive_ctor(QActive * const me, QStateHandler const initial);
void QActive_setAttr(QActive * const me, uint32_t attr, void const * const value);
/** @brief QACTIVE_START()'s record: the AO, its level, its queue and stack. */
void fake_kode_start(QActive * const me, uint8_t prio, size_t queue_len, size_t stack_bytes);

/** @brief Q_NEW_X(): a block while more than @p margin are free, else NULL. */
void *fake_kode_new(size_t size, uint16_t margin, enum_t sig);

/** @brief QACTIVE_POST_X(): queued while more than @p margin slots are free;
 *         otherwise false, and the block goes back to the pool. */
bool fake_kode_post(QActive * const me, QEvt const * const e, uint16_t margin,
                    void const * const sender);

#define QACTIVE_START(me_, prio_, queue_, len_, stack_, size_, par_)                     \
    ((void)(queue_), (void)(stack_), (void)(par_),                                     \
     fake_kode_start((me_), (uint8_t)(prio_), (len_), (size_)))

#define Q_NEW_X(evtT_, margin_, sig_)                                                    \
    ((evtT_ *)fake_kode_new(sizeof(evtT_), (uint16_t)(margin_), (enum_t)(sig_)))

#define QACTIVE_POST_X(me_, e_, margin_, sender_)                                        \
    fake_kode_post((me_), (e_), (uint16_t)(margin_), (sender_))


/* ---- The console's tables (kode_functions.h and kode_variables.h) ----------------------- */

#define KODE_ROW_NAME_MAX           15U
#define KODE_ROW_DESC_MAX           48U
#define KODE_VALUE_STR_MAX          32U
#define KODE_VARIABLES_MAX          32U
#define KODE_VARIABLE_TABLES        4U
#define KODE_FUNCTIONS_MAX          64U
#define KODE_FUNCTION_TABLES        4U
#define KODE_VALUE_ENUMS_MAX        6U
#define KODE_ARG_STR_MAX            256U
#define KODE_FUNCTION_ANSWER_MS_MAX 600000U

typedef enum {
    KODE_TYPE_NONE = 0,
    KODE_TYPE_INT,
    KODE_TYPE_BOOL,
    KODE_TYPE_ENUM,
    KODE_TYPE_STRING
} kode_type_t;

typedef struct {
    int32_t     i;
    bool        b;
    uint8_t     e;
    const char *s;
} kode_value_t;

typedef kode_status_t (*kode_function_run_fn)(const kode_value_t *arg);

typedef struct {
    const char           *name;
    const char           *desc;
    kode_type_t           arg;
    int32_t               min;
    int32_t               max;
    const char *const    *enums;
    uint8_t               n_enums;
    kode_function_run_fn  run;
    uint32_t              answer_ms;
} kode_function_t;

typedef kode_status_t (*kode_variable_get_fn)(char *buf, size_t cap);

typedef struct {
    const char           *name;
    const char           *desc;
    kode_variable_get_fn  get;
} kode_variable_t;

kode_status_t kode_functions_add(const kode_function_t *table, size_t n);
kode_status_t kode_variables_add(const kode_variable_t *table, size_t n);

#define KODE_FUNCTIONS_ADD(t) kode_functions_add((t), sizeof(t) / sizeof((t)[0]))
#define KODE_VARIABLES_ADD(t) kode_variables_add((t), sizeof(t) / sizeof((t)[0]))

#endif /* KODE_SYS_H_ */
