/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_idf.c
 * @brief ESP-IDF as the camera drivers call it, on the host. See fake_idf.h.
 */
#include "fake_idf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "camera_bsp.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "kode_log.h"

#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "ov5640.h"

fake_idf_t g_idf;

/* ---- Handles: what the driver is given back to hold ------------------------------- */

struct fake_i2c_bus { int tag; };
struct fake_i2c_dev { int tag; };
struct fake_sccb_io { int tag; };
struct fake_cam_ctlr { int tag; };
struct fake_jpeg_enc { int tag; };

static struct fake_i2c_bus  s_bus_h;
static struct fake_i2c_dev  s_dev_h;
static struct fake_sccb_io  s_io_h;
static struct fake_cam_ctlr s_cam_h;
static struct fake_jpeg_enc s_enc_h;

static esp_cam_sensor_device_t s_sensor;
static esp_cam_sensor_format_t s_formats[2];
static esp_cam_sensor_format_t s_fmt_now;

static shutdown_handler_t s_handlers[5];

void fake_idf_reset(void)
{
    memset(&g_idf, 0, sizeof(g_idf));
    for (int i = 0; i < FAKE_GPIOS; i++) {
        g_idf.direction[i] = -1;
        g_idf.pull[i]      = -1;
        g_idf.out_level[i] = -1;
    }
    g_idf.probe_ok        = true;
    g_idf.detect_ok       = true;
    g_idf.has_base_format = true;
    g_idf.format_name     = CAMERA_BSP_FORMAT;
    g_idf.write_fail_at   = -1;
    g_idf.buffer_fail_at  = -1;
    g_idf.process_size    = 900U;
    g_idf.heap_free       = 32U * 1024U * 1024U;
    fake_log_clear();   /* the handlers stay: the driver registers its own once, for good */
}

/* ---- Log ---------------------------------------------------------------------------------- */

#define LOG_LINES 32
#define LOG_WIDTH 160

static char     s_log[LOG_LINES][LOG_WIDTH];
static char     s_log_level[LOG_LINES];
static unsigned s_log_n;

void fake_log(char level, const char *tag, const char *fmt, ...)
{
    char line[LOG_WIDTH];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    unsigned const i = s_log_n % LOG_LINES;
    /* A long line is cut: the tests look for words near its start. */
    if (snprintf(s_log[i], LOG_WIDTH, "%s: %s", tag, line) < 0) {
        s_log[i][0] = '\0';
    }
    s_log_level[i] = level;
    s_log_n++;
}

void fake_log_clear(void)
{
    memset(s_log, 0, sizeof(s_log));
    memset(s_log_level, 0, sizeof(s_log_level));
    s_log_n = 0U;
}

bool fake_log_seen(const char *needle)
{
    for (unsigned i = 0U; i < LOG_LINES; i++) {
        if ((s_log_level[i] != '\0') && (strstr(s_log[i], needle) != NULL)) {
            return true;
        }
    }
    return false;
}

unsigned fake_log_count(char level)
{
    unsigned n = 0U;
    for (unsigned i = 0U; i < LOG_LINES; i++) {
        if (s_log_level[i] == level) {
            n++;
        }
    }
    return n;
}

/* ---- Pins ---------------------------------------------------------------------------------- */

static void op(fake_gpio_op_kind_t kind, int gpio, int arg)
{
    if (g_idf.n_ops < FAKE_GPIO_OPS) {
        g_idf.ops[g_idf.n_ops].kind = kind;
        g_idf.ops[g_idf.n_ops].gpio = gpio;
        g_idf.ops[g_idf.n_ops].arg  = arg;
        g_idf.n_ops++;
    }
}

static bool known(gpio_num_t g)
{
    return (g >= 0) && (g < FAKE_GPIOS);
}

esp_err_t gpio_set_direction(gpio_num_t g, gpio_mode_t mode)
{
    if (!known(g)) {
        return ESP_ERR_INVALID_ARG;
    }
    g_idf.direction[g] = (int)mode;
    op(OP_DIRECTION, g, (int)mode);
    return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t g, gpio_pull_mode_t pull)
{
    if (!known(g)) {
        return ESP_ERR_INVALID_ARG;
    }
    g_idf.pull[g] = (int)pull;
    op(OP_PULL, g, (int)pull);
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t g, uint32_t level)
{
    if (!known(g)) {
        return ESP_ERR_INVALID_ARG;
    }
    g_idf.out_level[g] = (int)level;
    op(OP_LEVEL, g, (int)level);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t g)
{
    if (!known(g)) {
        return 0;
    }
    unsigned const n = g_idf.gpio_reads++;
    if (g_idf.moving[g]) {
        return (int)(n & 1U);
    }
    return g_idf.in_level[g];
}

void fake_rom_delay_us(uint32_t us)
{
    g_idf.delay_us += us;
}

int64_t esp_timer_get_time(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return ((int64_t)t.tv_sec * 1000000) + ((int64_t)t.tv_nsec / 1000);
}

/* ---- The bus ----------------------------------------------------------------------------- */

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret)
{
    if (g_idf.bus_fail) {
        return ESP_FAIL;
    }
    g_idf.bus_new++;
    g_idf.bus_port   = cfg->i2c_port;
    g_idf.bus_sda    = cfg->sda_io_num;
    g_idf.bus_scl    = cfg->scl_io_num;
    g_idf.bus_pullup = cfg->flags.enable_internal_pullup != 0U;
    *ret = &s_bus_h;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    (void)bus;
    g_idf.bus_del++;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    (void)bus;
    (void)timeout_ms;
    g_idf.probes++;
    g_idf.probe_addr = address;
    return g_idf.probe_ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *ret)
{
    (void)bus;
    if (g_idf.dev_fail) {
        return ESP_FAIL;
    }
    g_idf.dev_add++;
    g_idf.dev_addr = cfg->device_address;
    *ret = &s_dev_h;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    (void)dev;
    g_idf.dev_rm++;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    g_idf.transmits++;
    g_idf.tx_len = len;
    memcpy(g_idf.tx, data, (len < sizeof(g_idf.tx)) ? len : sizeof(g_idf.tx));
    return ESP_OK;
}

/* ---- The SCCB -------------------------------------------------------------------------------- */

esp_err_t sccb_new_i2c_io(i2c_master_bus_handle_t bus, const sccb_i2c_config_t *cfg, esp_sccb_io_handle_t *ret)
{
    (void)bus;
    if (g_idf.io_fail) {
        return ESP_FAIL;
    }
    g_idf.io_new++;
    g_idf.io_hz = cfg->scl_speed_hz;
    *ret = &s_io_h;
    return ESP_OK;
}

esp_err_t esp_sccb_del_i2c_io(esp_sccb_io_handle_t h)
{
    (void)h;
    g_idf.io_del++;
    return ESP_OK;
}

esp_err_t esp_sccb_transmit_receive_reg_a16v8(esp_sccb_io_handle_t h, uint16_t reg, uint8_t *value)
{
    (void)h;
    g_idf.n_reads++;
    if (g_idf.read_fail) {
        return ESP_FAIL;
    }
    *value = g_idf.reg[reg];
    return ESP_OK;
}

esp_err_t esp_sccb_transmit_reg_a16v8(esp_sccb_io_handle_t h, uint16_t reg, uint8_t value)
{
    (void)h;
    unsigned const n = g_idf.n_writes;
    if ((g_idf.write_fail_at >= 0) && ((int)n >= g_idf.write_fail_at)) {
        g_idf.n_writes++;
        return ESP_FAIL;
    }
    if (n < FAKE_WRITES) {
        g_idf.writes[n].reg = reg;
        g_idf.writes[n].val = value;
    }
    g_idf.n_writes++;
    g_idf.reg[reg] = value;
    return ESP_OK;
}

esp_err_t esp_sccb_transmit_reg_a16(esp_sccb_io_handle_t h, uint16_t reg, const uint8_t *data, size_t n)
{
    (void)h;
    g_idf.bursts++;
    g_idf.burst_reg = reg;
    g_idf.burst_n   = n;
    memcpy(g_idf.burst_data, data, (n < sizeof(g_idf.burst_data)) ? n : sizeof(g_idf.burst_data));
    for (size_t i = 0U; (i < n) && ((reg + i) < 65536U); i++) {
        g_idf.reg[reg + i] = data[i];
    }
    return ESP_OK;
}

unsigned fake_idf_writes_to(uint16_t reg)
{
    unsigned n = 0U;
    for (unsigned i = 0U; (i < g_idf.n_writes) && (i < FAKE_WRITES); i++) {
        if (g_idf.writes[i].reg == reg) {
            n++;
        }
    }
    return n;
}

int fake_idf_last_write(uint16_t reg)
{
    int v = -1;
    for (unsigned i = 0U; (i < g_idf.n_writes) && (i < FAKE_WRITES); i++) {
        if (g_idf.writes[i].reg == reg) {
            v = g_idf.writes[i].val;
        }
    }
    return v;
}

/* ---- The sensor ------------------------------------------------------------------------------------ */

esp_cam_sensor_device_t *ov5640_detect(esp_cam_sensor_config_t *config)
{
    g_idf.detects++;
    if (!g_idf.detect_ok) {
        return NULL;
    }
    s_sensor.sccb_handle = config->sccb_handle;
    return &s_sensor;
}

esp_err_t esp_cam_sensor_query_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *out)
{
    (void)dev;
    if (g_idf.query_fail) {
        return ESP_FAIL;
    }
    /* Some of esp_cam_sensor's formats have no name at all: the first here. */
    s_formats[0] = (esp_cam_sensor_format_t){ .name = NULL };
    s_formats[1] = (esp_cam_sensor_format_t){ .name = g_idf.has_base_format ? CAMERA_BSP_FORMAT : "another" };
    out->count        = 2U;
    out->format_array = s_formats;
    return ESP_OK;
}

esp_err_t esp_cam_sensor_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *fmt)
{
    (void)dev;
    g_idf.set_formats++;
    g_idf.set_format_null = (fmt == NULL);
    return g_idf.set_format_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_cam_sensor_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *out)
{
    (void)dev;
    s_fmt_now      = (esp_cam_sensor_format_t){ .name = g_idf.format_name };
    *out           = s_fmt_now;
    return ESP_OK;
}

esp_err_t esp_cam_sensor_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    (void)dev;
    if (cmd != ESP_CAM_SENSOR_IOC_S_STREAM) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    g_idf.streams++;
    if (g_idf.streams <= g_idf.stream_fail_first) {
        return ESP_FAIL;
    }
    g_idf.stream_last = *(int *)arg;
    return ESP_OK;
}

esp_err_t esp_cam_sensor_del_dev(esp_cam_sensor_device_t *dev)
{
    (void)dev;
    g_idf.dev_del++;
    return ESP_OK;
}

/* ---- The heap ------------------------------------------------------------------------------------------ */

void *fake_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    void *p = NULL;

    g_idf.heap_calls++;
    g_idf.heap_align = (uint32_t)alignment;
    if (caps != 0U) {
        g_idf.heap_caps = caps;   /* the encoder's own blocks ask for none */
    }
    if ((g_idf.heap_fail_nth != 0U) && (g_idf.heap_calls >= g_idf.heap_fail_nth)) {
        return NULL;
    }
    if (posix_memalign(&p, (alignment < sizeof(void *)) ? sizeof(void *) : alignment, size) != 0) {
        return NULL;
    }
    g_idf.heap_live++;
    return p;
}

/* A plain block, as the heap gives it: on 8 bytes only, never on a cache line
 * (8 past one), so that a caller that needs a line has to find it itself. */
void *fake_heap_caps_malloc(size_t size, uint32_t caps)
{
    void *p = NULL;

    g_idf.heap_calls++;
    g_idf.heap_caps        = caps;
    g_idf.heap_malloc_size = size;
    if ((g_idf.heap_fail_nth != 0U) && (g_idf.heap_calls >= g_idf.heap_fail_nth)) {
        return NULL;
    }
    if (posix_memalign(&p, 128U, size + 8U) != 0) {
        return NULL;
    }
    g_idf.heap_live++;
    return (uint8_t *)p + 8U;
}

void fake_heap_caps_free(void *ptr)
{
    if (ptr != NULL) {
        g_idf.heap_live--;
        free((((uintptr_t)ptr % 16U) == 8U) ? (void *)((uint8_t *)ptr - 8U) : ptr);
    }
}

size_t fake_heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return g_idf.heap_free;
}

size_t fake_heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return g_idf.heap_free / 2U;
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    (void)addr;
    (void)flags;
    g_idf.msyncs++;
    g_idf.msync_size = size;
    return ESP_OK;
}

/* ---- The camera controller -------------------------------------------------------------------------------- */

esp_err_t esp_cam_new_dvp_ctlr(const esp_cam_ctlr_dvp_config_t *cfg, esp_cam_ctlr_handle_t *ret)
{
    if (g_idf.ctlr_fail) {
        return ESP_ERR_INVALID_STATE;
    }
    g_idf.ctlr_new++;
    g_idf.ctlr_id = cfg->ctlr_id;
    g_idf.h_res   = cfg->h_res;
    g_idf.v_res   = cfg->v_res;
    g_idf.vsync   = cfg->pin->vsync_io;
    g_idf.de      = cfg->pin->de_io;
    g_idf.pclk    = cfg->pin->pclk_io;
    g_idf.xclk    = cfg->pin->xclk_io;
    for (int i = 0; i < 8; i++) {
        g_idf.data[i] = cfg->pin->data_io[i];
    }
    g_idf.has_cbs = false;
    *ret = &s_cam_h;
    return ESP_OK;
}

esp_err_t esp_cam_ctlr_register_event_callbacks(esp_cam_ctlr_handle_t h, const esp_cam_ctlr_evt_cbs_t *cbs,
                                                void *user_data)
{
    (void)h;
    if (g_idf.cbs_fail) {
        return ESP_FAIL;
    }
    g_idf.cbs      = *cbs;
    g_idf.cbs_user = user_data;
    g_idf.has_cbs  = true;
    return ESP_OK;
}

esp_err_t esp_cam_ctlr_enable(esp_cam_ctlr_handle_t h)
{
    (void)h;
    g_idf.enables++;
    return g_idf.enable_fail ? (esp_err_t)g_idf.enable_rc : ESP_OK;
}

esp_err_t esp_cam_ctlr_disable(esp_cam_ctlr_handle_t h)
{
    (void)h;
    g_idf.disables++;
    return ESP_OK;
}

esp_err_t esp_cam_ctlr_start(esp_cam_ctlr_handle_t h)
{
    (void)h;
    g_idf.starts++;
    return g_idf.start_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_cam_ctlr_stop(esp_cam_ctlr_handle_t h)
{
    (void)h;
    g_idf.stops++;
    return ESP_OK;
}

esp_err_t esp_cam_ctlr_del(esp_cam_ctlr_handle_t h)
{
    (void)h;
    g_idf.ctlr_del++;
    g_idf.has_cbs = false;
    return ESP_OK;
}

void *esp_cam_ctlr_alloc_buffer(esp_cam_ctlr_handle_t h, size_t size, uint32_t heap_caps)
{
    (void)h;
    unsigned const n = g_idf.cam_bufs;
    if ((g_idf.buffer_fail_at >= 0) && ((int)n >= g_idf.buffer_fail_at)) {
        return NULL;
    }
    void *p = fake_heap_caps_aligned_alloc(64U, size, heap_caps);
    if ((p != NULL) && (n < FAKE_CAM_BUFS)) {
        g_idf.cam_buf[n] = p;
        g_idf.cam_bufs   = n + 1U;
    }
    return p;
}

static esp_cam_ctlr_trans_t s_trans;

void fake_cam_new(void)
{
    if (g_idf.has_cbs && (g_idf.cbs.on_get_new_trans != NULL)) {
        s_trans = (esp_cam_ctlr_trans_t){ 0 };
        (void)g_idf.cbs.on_get_new_trans(&s_cam_h, &s_trans, g_idf.cbs_user);
    }
}

bool fake_cam_frame(void)
{
    if (!g_idf.has_cbs) {
        return false;
    }
    /* The DMA finishes the buffer it was filling - the one the driver's context says it
     * was given: the context of the DVP driver starts with the transaction it hands out -
     * and then asks for the next. */
    esp_cam_ctlr_trans_t done = { 0 };
    if (g_idf.cbs_user != NULL) {
        done = *(const esp_cam_ctlr_trans_t *)g_idf.cbs_user;
    }
    bool const yield = (g_idf.cbs.on_trans_finished != NULL)
                           ? g_idf.cbs.on_trans_finished(&s_cam_h, &done, g_idf.cbs_user) : false;
    fake_cam_new();
    return yield;
}

/* ---- The JPEG engine ------------------------------------------------------------------------------------------ */

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg, jpeg_encoder_handle_t *ret)
{
    (void)cfg;
    if (g_idf.eng_fail) {
        return ESP_FAIL;
    }
    g_idf.eng_new++;
    *ret = &s_enc_h;
    return ESP_OK;
}

esp_err_t jpeg_del_encoder_engine(jpeg_encoder_handle_t enc)
{
    (void)enc;
    g_idf.eng_del++;
    return ESP_OK;
}

void *jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    g_idf.jpeg_allocs++;
    g_idf.alloc_asked = size;
    g_idf.alloc_dir   = (int)cfg->buffer_direction;
    if (g_idf.jpeg_alloc_fail) {
        return NULL;
    }
    /* The encoder rounds up to its cache line: the caller must use what it was told. */
    size_t const got = ((size + 127U) / 128U) * 128U;
    void *p = fake_heap_caps_aligned_alloc(128U, got, 0U);
    if (p != NULL) {
        *allocated      = got;
        g_idf.alloc_got = got;
    }
    return p;
}

esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t enc, const jpeg_encode_cfg_t *cfg, const uint8_t *in,
                               uint32_t in_size, uint8_t *out, uint32_t out_cap, uint32_t *out_size)
{
    (void)enc;
    g_idf.process_calls++;
    g_idf.last_cfg      = *cfg;
    g_idf.last_in       = in;
    g_idf.last_in_size  = in_size;
    g_idf.last_out      = out;
    g_idf.last_out_cap  = out_cap;
    if (g_idf.process_calls <= g_idf.process_fail_first) {
        return ESP_FAIL;
    }
    uint32_t const n = g_idf.process_size;
    if (n > out_cap) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n >= 4U) {
        memset(out, 0x5A, n);
        out[0]     = 0xFFU;
        out[1]     = 0xD8U;
        out[n - 2U] = 0xFFU;
        out[n - 1U] = 0xD9U;
    }
    *out_size = g_idf.process_zero ? 0U : n;
    return ESP_OK;
}

/* ---- Shutdown handlers ------------------------------------------------------------------------------------------ */

esp_err_t fake_register_shutdown_handler(shutdown_handler_t handle)
{
    for (unsigned i = 0U; i < 5U; i++) {
        if (s_handlers[i] == handle) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (g_idf.sd_full) {
        return ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0U; i < 5U; i++) {
        if (s_handlers[i] == NULL) {
            s_handlers[i] = handle;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

void fake_shutdown_run(void)
{
    for (unsigned i = 0U; i < 5U; i++) {
        if (s_handlers[i] != NULL) {
            s_handlers[i]();
        }
    }
}

unsigned fake_shutdown_count(void)
{
    unsigned n = 0U;
    for (unsigned i = 0U; i < 5U; i++) {
        if (s_handlers[i] != NULL) {
            n++;
        }
    }
    return n;
}
