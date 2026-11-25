/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_idf.h
 * @brief What the camera drivers stand on, played on the host: pins that can be
 *        read and driven, an I2C bus with an OV5640 behind it (64 K registers),
 *        a camera controller whose frames the case fires by hand, a JPEG engine,
 *        a PSRAM that can run out. Every call is written down and every failure
 *        is the case's to set; fake_idf_reset() clears all of it.
 */
#ifndef FAKE_IDF_H_
#define FAKE_IDF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_cam_sensor.h"
#include "esp_heap_caps.h"
#include "kode_log.h"

#define FAKE_GPIOS     64
#define FAKE_GPIO_OPS  64
#define FAKE_WRITES    256
#define FAKE_CAM_BUFS  8

typedef enum { OP_DIRECTION, OP_PULL, OP_LEVEL } fake_gpio_op_kind_t;

typedef struct {
    fake_gpio_op_kind_t kind;
    int                 gpio;
    int                 arg;   /**< the mode, the pull or the level */
} fake_gpio_op_t;

typedef struct {
    uint16_t reg;
    uint8_t  val;
} fake_write_t;

typedef struct {
    /* ---- Pins ------------------------------------------------------------ */
    int            in_level[FAKE_GPIOS];   /**< What a read finds.                          */
    bool           moving[FAKE_GPIOS];     /**< ... or alternates 0, 1, 0, 1...             */
    int            direction[FAKE_GPIOS];  /**< -1 until set.                               */
    int            pull[FAKE_GPIOS];       /**< -1 until set.                               */
    int            out_level[FAKE_GPIOS];  /**< -1 until set.                               */
    unsigned       gpio_reads;
    fake_gpio_op_t ops[FAKE_GPIO_OPS];
    unsigned       n_ops;
    uint32_t       delay_us;               /**< esp_rom_delay_us() added up.                */

    /* ---- The bus ---------------------------------------------------------- */
    bool           bus_fail;
    unsigned       bus_new;
    unsigned       bus_del;
    int            bus_port;
    int            bus_sda;
    int            bus_scl;
    bool           bus_pullup;
    bool           probe_ok;
    unsigned       probes;
    uint16_t       probe_addr;
    bool           dev_fail;
    unsigned       dev_add;
    unsigned       dev_rm;
    uint16_t       dev_addr;
    unsigned       transmits;
    uint8_t        tx[8];
    size_t         tx_len;

    /* ---- The SCCB and the registers ----------------------------------------- */
    bool           io_fail;
    unsigned       io_new;
    unsigned       io_del;
    uint32_t       io_hz;
    uint8_t        reg[65536];
    fake_write_t   writes[FAKE_WRITES];    /**< Every single-register write, in order.      */
    unsigned       n_writes;
    unsigned       n_reads;
    int            write_fail_at;          /**< The write (0-based) that fails, and the rest; -1 none. */
    bool           read_fail;
    unsigned       bursts;
    uint16_t       burst_reg;
    uint8_t        burst_data[300];
    size_t         burst_n;

    /* ---- The sensor ----------------------------------------------------------- */
    bool           detect_ok;
    unsigned       detects;
    const char    *format_name;            /**< What the sensor reports it took.            */
    bool           has_base_format;        /**< The base format is in its list.             */
    bool           query_fail;
    bool           set_format_fail;
    unsigned       set_formats;
    bool           set_format_null;        /**< The format asked was NULL (as the driver must). */
    unsigned       dev_del;
    unsigned       streams;
    int            stream_last;
    unsigned       stream_fail_first;      /**< The first n stream ioctls fail.            */

    /* ---- The camera controller ---------------------------------------------------- */
    bool           ctlr_fail;
    bool           cbs_fail;
    bool           enable_fail;
    int            enable_rc;              /**< Answer of enable() when it is made to fail. */
    bool           start_fail;
    int            buffer_fail_at;         /**< The buffer (0-based) that runs out; -1 none. */
    unsigned       ctlr_new;
    unsigned       ctlr_del;
    unsigned       enables;
    unsigned       disables;
    unsigned       starts;
    unsigned       stops;
    unsigned       cam_bufs;
    void          *cam_buf[FAKE_CAM_BUFS];
    int            ctlr_id;
    uint32_t       h_res;
    uint32_t       v_res;
    gpio_num_t     vsync;
    gpio_num_t     de;
    gpio_num_t     pclk;
    gpio_num_t     xclk;
    gpio_num_t     data[8];
    bool           has_cbs;
    unsigned       msyncs;
    size_t         msync_size;
    esp_cam_ctlr_evt_cbs_t cbs;
    void          *cbs_user;

    /* ---- The JPEG engine ----------------------------------------------------------- */
    bool           eng_fail;
    unsigned       eng_new;
    unsigned       eng_del;
    bool           jpeg_alloc_fail;
    unsigned       jpeg_allocs;
    size_t         alloc_asked;
    size_t         alloc_got;
    int            alloc_dir;
    unsigned       process_calls;
    unsigned       process_fail_first;
    bool           process_zero;
    uint32_t       process_size;           /**< The JPEG it makes.                          */
    jpeg_encode_cfg_t last_cfg;
    const uint8_t *last_in;
    uint32_t       last_in_size;
    uint8_t       *last_out;
    uint32_t       last_out_cap;

    /* ---- The heap -------------------------------------------------------------------- */
    unsigned       heap_live;              /**< Allocations not yet freed.                  */
    unsigned       heap_calls;             /**< heap_caps_aligned_alloc() and heap_caps_malloc() calls. */
    unsigned       heap_fail_nth;          /**< That call (1-based) and the rest fail; 0 none. */
    size_t         heap_free;              /**< What heap_caps_get_free_size() says.        */
    uint32_t       heap_align;             /**< Alignment of the last request.              */
    uint32_t       heap_caps;              /**< Caps of the last request.                   */
    size_t         heap_malloc_size;       /**< Size of the last heap_caps_malloc().        */

    /* ---- The shutdown handlers ----------------------------------------------------------- */
    bool           sd_full;
} fake_idf_t;

extern fake_idf_t g_idf;

/** @brief Everything cleared: pins unset, an OV5640 that answers with the base format, memory plenty. */
void fake_idf_reset(void);

/** @brief The registers written, as "reg:val" pairs in order, for a failure message. */
unsigned fake_idf_writes_to(uint16_t reg);

/** @brief The last value written to @p reg, or -1 when it was not. */
int fake_idf_last_write(uint16_t reg);

/** @brief One frame: the DMA finishes its buffer, then asks for the next. Whether the
 *         callback asked for a task switch. */
bool fake_cam_frame(void);

/** @brief The controller asks for the next buffer only. */
void fake_cam_new(void);

/** @brief esp_restart()'s part: every handler registered, in order. */
void fake_shutdown_run(void);

/** @brief Handlers held now; not cleared by fake_idf_reset(): a driver registers its own once. */
unsigned fake_shutdown_count(void);

/** @brief Forget the log. */
void fake_log_clear(void);

#endif /* FAKE_IDF_H_ */
