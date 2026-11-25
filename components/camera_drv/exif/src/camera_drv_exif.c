/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_exif.c
 * @brief The EXIF a photo carries. See camera_drv_exif.h.
 *
 * ONE PASS TWICE. The segment's length must be known before a byte is
 * written - a buffer one byte short has to be refused untouched - and the
 * directories point at values whose places depend on how many entries came
 * before. So layout() is written once and run twice: first measuring (no
 * buffer, it only tracks how far it would have written), then writing into
 * a buffer of exactly that size. Both runs take the same path, so they
 * cannot disagree.
 */
#include "camera_drv_exif.h"

#include <stddef.h>
#include <string.h>

/* ---- TIFF ------------------------------------------------------------------------ */

#define TYPE_BYTE       1U
#define TYPE_ASCII      2U
#define TYPE_SHORT      3U
#define TYPE_LONG       4U
#define TYPE_RATIONAL   5U
#define TYPE_UNDEFINED  7U

/* IFD0 */
#define TAG_MAKE          0x010FU
#define TAG_MODEL         0x0110U
#define TAG_ORIENTATION   0x0112U
#define TAG_XRES          0x011AU
#define TAG_YRES          0x011BU
#define TAG_RES_UNIT      0x0128U
#define TAG_SOFTWARE      0x0131U
#define TAG_DATETIME      0x0132U
#define TAG_EXIF_PTR      0x8769U
#define TAG_GPS_PTR       0x8825U
/* Exif IFD */
#define TAG_EXPOSURE      0x829AU
#define TAG_ISO           0x8827U
#define TAG_EXIF_VERSION  0x9000U
#define TAG_DT_ORIGINAL   0x9003U
#define TAG_DT_DIGITIZED  0x9004U
#define TAG_OFFSET        0x9010U
#define TAG_OFFSET_ORIG   0x9011U
#define TAG_OFFSET_DIGIT  0x9012U
#define TAG_COLOR_SPACE   0xA001U
#define TAG_PIXEL_X       0xA002U
#define TAG_PIXEL_Y       0xA003U
/* GPS IFD */
#define TAG_GPS_VERSION   0x0000U
#define TAG_GPS_LAT_REF   0x0001U
#define TAG_GPS_LAT       0x0002U
#define TAG_GPS_LON_REF   0x0003U
#define TAG_GPS_LON       0x0004U
#define TAG_GPS_ALT_REF   0x0005U
#define TAG_GPS_ALT       0x0006U
#define TAG_GPS_TIME      0x0007U
#define TAG_GPS_DATE      0x001DU

#define TIFF_HEAD_LEN     8U     /* "II", 42, offset of IFD0                        */
#define APP1_HEAD_LEN     10U    /* 0xFF 0xE1, length, "Exif\0\0"                   */
#define APP1_MAX_LEN      0xFFFFU
#define ENTRY_LEN         12U
#define STR_BUF           (CAMERA_DRV_EXIF_STRING_MAX + 1U)
#define DATETIME_LEN      20U    /* "YYYY:MM:DD HH:MM:SS" and its NUL               */
#define DATESTAMP_LEN     11U    /* "YYYY:MM:DD" and its NUL                        */
#define OFFSET_LEN        7U     /* "+HH:MM" and its NUL                            */
#define OFFSET_MIN_MIN    (-720) /* -12:00, the furthest west a clock is set        */
#define OFFSET_MAX_MIN    840    /* +14:00, the furthest east                       */
#define E7                10000000U
#define SEC_DEN           1000000U /* seconds of arc: 60 / 1e7 degrees x 1e6 = 6 exact */

#define LAT_MAX_E7        900000000
#define LON_MAX_E7        1800000000

static const uint8_t EXIF_ID[6] = { 'E', 'x', 'i', 'f', 0U, 0U };

/* ---- A writer that can only measure ---------------------------------------------- */

/* Offsets here count from the first byte of the TIFF header. */
typedef struct {
    uint8_t *out;  /* the TIFF header's place, or NULL to measure only     */
    size_t   cap;  /* bytes available from out                             */
    size_t   len;  /* the furthest byte written, or that would be          */
    bool     fail; /* a write fell outside cap                             */
} wr_t;

static void w_put(wr_t *w, size_t off, const uint8_t *src, size_t n)
{
    if ((off + n) > w->len) {
        w->len = off + n;
    }
    if (w->out != NULL) {
        if ((off + n) > w->cap) {
            w->fail = true;
            return;
        }
        (void)memcpy(&w->out[off], src, n);
    }
}

static void put16(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFFU);
    b[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put32(uint8_t *b, uint32_t v)
{
    put16(b, v & 0xFFFFU);
    put16(&b[2], v >> 16);
}

/* ---- One directory ---------------------------------------------------------------- */

typedef struct {
    wr_t   *w;
    size_t  at;    /* where the next entry goes                            */
    size_t *data;  /* the next free byte of the data area, shared by all   */
} ifd_t;

static size_t ifd_size(uint32_t entries)
{
    return 2U + ((size_t)entries * ENTRY_LEN) + 4U;
}

static void ifd_open(ifd_t *f, wr_t *w, size_t off, uint32_t entries, size_t *data)
{
    uint8_t b[2];

    put16(b, entries);
    w_put(w, off, b, sizeof(b));
    f->w = w;
    f->at = off + 2U;
    f->data = data;
}

/* The next-IFD offset: 0, there is no thumbnail. */
static void ifd_close(ifd_t *f)
{
    static const uint8_t zero[4] = { 0U, 0U, 0U, 0U };

    w_put(f->w, f->at, zero, sizeof(zero));
}

/* One entry. @p v holds the value already in the file's byte order; up to four
 * bytes go in the entry itself, more in the data area, at an even offset. */
static void ifd_add(ifd_t *f, uint16_t tag, uint16_t type, uint32_t count,
                    const uint8_t *v, size_t n)
{
    uint8_t e[ENTRY_LEN] = { 0U };

    put16(e, tag);
    put16(&e[2], type);
    put32(&e[4], count);
    if (n <= 4U) {
        (void)memcpy(&e[8], v, n);
    } else {
        size_t const at = *f->data;

        put32(&e[8], (uint32_t)at);
        w_put(f->w, at, v, n);
        *f->data = at + ((n + 1U) & ~(size_t)1U);
    }
    w_put(f->w, f->at, e, sizeof(e));
    f->at += ENTRY_LEN;
}

static void ifd_short(ifd_t *f, uint16_t tag, uint32_t v)
{
    uint8_t b[2];

    put16(b, v);
    ifd_add(f, tag, (uint16_t)TYPE_SHORT, 1U, b, sizeof(b));
}

static void ifd_long(ifd_t *f, uint16_t tag, uint32_t v)
{
    uint8_t b[4];

    put32(b, v);
    ifd_add(f, tag, (uint16_t)TYPE_LONG, 1U, b, sizeof(b));
}

static void ifd_bytes(ifd_t *f, uint16_t tag, uint16_t type, const uint8_t *v, uint32_t n)
{
    ifd_add(f, tag, type, n, v, n);
}

/* @p n rationals from @p r, numerator then denominator each. */
static void ifd_rationals(ifd_t *f, uint16_t tag, const uint32_t *r, uint32_t n)
{
    uint8_t b[24];

    for (uint32_t i = 0U; i < (n * 2U); i++) {
        put32(&b[i * 4U], r[i]);
    }
    ifd_add(f, tag, (uint16_t)TYPE_RATIONAL, n, b, (size_t)n * 8U);
}

static void ifd_ascii(ifd_t *f, uint16_t tag, const char *s, uint32_t n)
{
    ifd_add(f, tag, (uint16_t)TYPE_ASCII, n, (const uint8_t *)s, n);
}

/* ---- Values ----------------------------------------------------------------------- */

static bool leap(uint32_t y)
{
    return (((y % 4U) == 0U) && ((y % 100U) != 0U)) || ((y % 400U) == 0U);
}

/* A date and time that exist. Year 0 is "unknown" and is the caller's to test. */
static bool time_ok(const camera_drv_exif_time_t *t)
{
    static const uint8_t days[12] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };
    uint32_t dmax;

    if ((t->year < 1U) || (t->year > 9999U) || (t->month < 1U) || (t->month > 12U)) {
        return false;
    }
    dmax = days[t->month - 1U];
    if ((t->month == 2U) && leap(t->year)) {
        dmax = 29U;
    }
    return (t->day >= 1U) && (t->day <= dmax) && (t->hour <= 23U) && (t->minute <= 59U) &&
           (t->second <= 59U);
}

static void dec(char *dst, uint32_t v, uint32_t digits)
{
    for (uint32_t i = digits; i > 0U; i--) {
        dst[i - 1U] = (char)('0' + (char)(v % 10U));
        v /= 10U;
    }
}

/* "YYYY:MM:DD HH:MM:SS" and its NUL, 20 bytes. */
static void fmt_datetime(char *dst, const camera_drv_exif_time_t *t)
{
    dec(&dst[0], t->year, 4U);
    dst[4] = ':';
    dec(&dst[5], t->month, 2U);
    dst[7] = ':';
    dec(&dst[8], t->day, 2U);
    dst[10] = ' ';
    dec(&dst[11], t->hour, 2U);
    dst[13] = ':';
    dec(&dst[14], t->minute, 2U);
    dst[16] = ':';
    dec(&dst[17], t->second, 2U);
    dst[19] = '\0';
}

/* "YYYY:MM:DD" and its NUL, 11 bytes. */
static void fmt_date(char *dst, const camera_drv_exif_time_t *t)
{
    dec(&dst[0], t->year, 4U);
    dst[4] = ':';
    dec(&dst[5], t->month, 2U);
    dst[7] = ':';
    dec(&dst[8], t->day, 2U);
    dst[10] = '\0';
}

/* The string, cut and cleaned, with its NUL. Returns the length counting the NUL. */
static uint32_t str_prep(char *dst, const char *s)
{
    uint32_t n = 0U;

    while ((n < CAMERA_DRV_EXIF_STRING_MAX) && (s[n] != '\0')) {
        char const c = s[n];

        dst[n] = ((c >= 0x20) && (c <= 0x7E)) ? c : '?';
        n++;
    }
    dst[n] = '\0';
    return n + 1U;
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b != 0U) {
        uint32_t const t = a % b;

        a = b;
        b = t;
    }
    return a;
}

static uint32_t abs_u32(int32_t v)
{
    return (v < 0) ? ((uint32_t)0U - (uint32_t)v) : (uint32_t)v;
}

/* "+HH:MM" and its NUL, 7 bytes: how far the clock is ahead of UTC. 0 is "+00:00". */
static void fmt_offset(char *dst, int16_t minutes)
{
    uint32_t const m = abs_u32((int32_t)minutes);

    dst[0] = (minutes < 0) ? '-' : '+';
    dec(&dst[1], m / 60U, 2U);
    dst[3] = ':';
    dec(&dst[4], m % 60U, 2U);
    dst[6] = '\0';
}

/* Degrees x 1e7 -> degrees, minutes and seconds as three rationals. The seconds
 * are exact: the remainder of a minute, in 1e-7 of a degree, times 6, over 1e6. */
static void dms(uint32_t e7, uint32_t r[6])
{
    uint32_t rem = e7 % E7;

    rem *= 60U;   /* at most 599999940: fits */
    r[0] = e7 / E7;
    r[1] = 1U;
    r[2] = rem / E7;
    r[3] = 1U;
    r[4] = (rem % E7) * 6U;
    r[5] = SEC_DEN;
}

/* ---- The layout ------------------------------------------------------------------- */

typedef struct {
    char     make[STR_BUF];
    char     model[STR_BUF];
    char     software[STR_BUF];
    uint32_t make_n;
    uint32_t model_n;
    uint32_t software_n;
} strs_t;

static bool inputs_ok(const camera_drv_exif_t *e)
{
    if ((e->orientation < 1U) || (e->orientation > 8U)) {
        return false;
    }
    if ((e->taken.year != 0U) && !time_ok(&e->taken)) {
        return false;
    }
    if ((e->taken.year != 0U) && e->has_offset &&
        ((e->offset_min < OFFSET_MIN_MIN) || (e->offset_min > OFFSET_MAX_MIN))) {
        return false;
    }
    if (e->gps.valid) {
        if ((e->gps.lat_e7 < -LAT_MAX_E7) || (e->gps.lat_e7 > LAT_MAX_E7) ||
            (e->gps.lon_e7 < -LON_MAX_E7) || (e->gps.lon_e7 > LON_MAX_E7)) {
            return false;
        }
        if ((e->gps.utc.year != 0U) && !time_ok(&e->gps.utc)) {
            return false;
        }
    }
    return true;
}

static void write_ifd0(ifd_t *f, const camera_drv_exif_t *e, const strs_t *s, bool date, bool gps,
                       size_t exif_off, size_t gps_off)
{
    static const uint32_t DPI[2] = { 72U, 1U };
    char dt[DATETIME_LEN];

    ifd_ascii(f, (uint16_t)TAG_MAKE, s->make, s->make_n);
    ifd_ascii(f, (uint16_t)TAG_MODEL, s->model, s->model_n);
    ifd_short(f, (uint16_t)TAG_ORIENTATION, e->orientation);
    ifd_rationals(f, (uint16_t)TAG_XRES, DPI, 1U);
    ifd_rationals(f, (uint16_t)TAG_YRES, DPI, 1U);
    ifd_short(f, (uint16_t)TAG_RES_UNIT, 2U);   /* inches */
    if (e->software != NULL) {
        ifd_ascii(f, (uint16_t)TAG_SOFTWARE, s->software, s->software_n);
    }
    if (date) {
        fmt_datetime(dt, &e->taken);
        ifd_ascii(f, (uint16_t)TAG_DATETIME, dt, DATETIME_LEN);
    }
    ifd_long(f, (uint16_t)TAG_EXIF_PTR, (uint32_t)exif_off);
    if (gps) {
        ifd_long(f, (uint16_t)TAG_GPS_PTR, (uint32_t)gps_off);
    }
    ifd_close(f);
}

static void write_exif_ifd(ifd_t *f, const camera_drv_exif_t *e, bool date)
{
    static const uint8_t VERSION[4] = { '0', '2', '3', '2' };
    char dt[DATETIME_LEN];

    if (e->exposure_us != 0U) {
        /* The exact fraction of a second, reduced: 10000 us is 1/100. */
        uint32_t const g = gcd(e->exposure_us, 1000000U);
        uint32_t const r[2] = { e->exposure_us / g, 1000000U / g };

        ifd_rationals(f, (uint16_t)TAG_EXPOSURE, r, 1U);
    }
    if (e->iso != 0U) {
        ifd_short(f, (uint16_t)TAG_ISO, e->iso);
    }
    ifd_bytes(f, (uint16_t)TAG_EXIF_VERSION, (uint16_t)TYPE_UNDEFINED, VERSION, 4U);
    if (date) {
        fmt_datetime(dt, &e->taken);
        ifd_ascii(f, (uint16_t)TAG_DT_ORIGINAL, dt, DATETIME_LEN);
        ifd_ascii(f, (uint16_t)TAG_DT_DIGITIZED, dt, DATETIME_LEN);
        if (e->has_offset) {
            char off[OFFSET_LEN];

            fmt_offset(off, e->offset_min);
            ifd_ascii(f, (uint16_t)TAG_OFFSET, off, OFFSET_LEN);
            ifd_ascii(f, (uint16_t)TAG_OFFSET_ORIG, off, OFFSET_LEN);
            ifd_ascii(f, (uint16_t)TAG_OFFSET_DIGIT, off, OFFSET_LEN);
        }
    }
    ifd_short(f, (uint16_t)TAG_COLOR_SPACE, 1U);   /* sRGB */
    if ((e->width != 0U) && (e->height != 0U)) {
        ifd_long(f, (uint16_t)TAG_PIXEL_X, e->width);
        ifd_long(f, (uint16_t)TAG_PIXEL_Y, e->height);
    }
    ifd_close(f);
}

static void write_gps_ifd(ifd_t *f, const camera_drv_exif_gps_t *g)
{
    static const uint8_t VERSION[4] = { 2U, 3U, 0U, 0U };
    uint32_t r[6];
    char ref[2] = { 'N', '\0' };
    uint8_t alt_ref;

    ifd_bytes(f, (uint16_t)TAG_GPS_VERSION, (uint16_t)TYPE_BYTE, VERSION, 4U);

    ref[0] = (g->lat_e7 < 0) ? 'S' : 'N';
    ifd_ascii(f, (uint16_t)TAG_GPS_LAT_REF, ref, 2U);
    dms(abs_u32(g->lat_e7), r);
    ifd_rationals(f, (uint16_t)TAG_GPS_LAT, r, 3U);

    ref[0] = (g->lon_e7 < 0) ? 'W' : 'E';
    ifd_ascii(f, (uint16_t)TAG_GPS_LON_REF, ref, 2U);
    dms(abs_u32(g->lon_e7), r);
    ifd_rationals(f, (uint16_t)TAG_GPS_LON, r, 3U);

    if (g->alt_valid) {
        uint32_t const alt[2] = { abs_u32(g->alt_mm), 1000U };

        alt_ref = (g->alt_mm < 0) ? 1U : 0U;   /* 1 = below sea level */
        ifd_bytes(f, (uint16_t)TAG_GPS_ALT_REF, (uint16_t)TYPE_BYTE, &alt_ref, 1U);
        ifd_rationals(f, (uint16_t)TAG_GPS_ALT, alt, 1U);
    }
    if (g->utc.year != 0U) {
        char date[DATESTAMP_LEN];
        uint32_t const t[6] = { g->utc.hour, 1U, g->utc.minute, 1U, g->utc.second, 1U };

        ifd_rationals(f, (uint16_t)TAG_GPS_TIME, t, 3U);
        fmt_date(date, &g->utc);
        ifd_ascii(f, (uint16_t)TAG_GPS_DATE, date, DATESTAMP_LEN);
    }
    ifd_close(f);
}

/* The whole segment. With @p out NULL it only measures. Returns its length, 0 on
 * a refusal. With @p out set, @p cap must be what the measuring run returned. */
static size_t layout(const camera_drv_exif_t *e, uint8_t *out, size_t cap)
{
    static const uint8_t TIFF_HEAD[TIFF_HEAD_LEN] = { 'I', 'I', 42U, 0U, 8U, 0U, 0U, 0U };
    strs_t s;
    wr_t w;
    ifd_t f;
    bool const date = (e->taken.year != 0U);
    bool const gps = e->gps.valid;
    bool const gps_date = gps && (e->gps.utc.year != 0U);
    bool const dim = (e->width != 0U) && (e->height != 0U);
    uint32_t n0;
    uint32_t n1;
    uint32_t n2 = 0U;
    size_t exif_off;
    size_t gps_off;
    size_t data;
    size_t total;

    if (!inputs_ok(e)) {
        return 0U;
    }
    s.make_n = str_prep(s.make, (e->make != NULL) ? e->make : "Kode");
    s.model_n = str_prep(s.model, (e->model != NULL) ? e->model : "Kode Dot Camera");
    s.software_n = (e->software != NULL) ? str_prep(s.software, e->software) : 0U;

    /* How many entries each directory has, so each one's place is known before
     * it is written. Keep in step with the write_* functions above. */
    n0 = 7U + ((e->software != NULL) ? 1U : 0U) + (date ? 1U : 0U) + (gps ? 1U : 0U);
    n1 = 2U + (date ? 2U : 0U) + ((date && e->has_offset) ? 3U : 0U) +
         ((e->exposure_us != 0U) ? 1U : 0U) +
         ((e->iso != 0U) ? 1U : 0U) + (dim ? 2U : 0U);
    if (gps) {
        n2 = 5U + (e->gps.alt_valid ? 2U : 0U) + (gps_date ? 2U : 0U);
    }
    exif_off = TIFF_HEAD_LEN + ifd_size(n0);
    gps_off = exif_off + ifd_size(n1);
    data = gps ? (gps_off + ifd_size(n2)) : gps_off;

    w.out = (out != NULL) ? &out[APP1_HEAD_LEN] : NULL;
    w.cap = (out != NULL) ? (cap - APP1_HEAD_LEN) : 0U;
    w.len = 0U;
    w.fail = false;

    w_put(&w, 0U, TIFF_HEAD, TIFF_HEAD_LEN);
    ifd_open(&f, &w, TIFF_HEAD_LEN, n0, &data);
    write_ifd0(&f, e, &s, date, gps, exif_off, gps_off);
    ifd_open(&f, &w, exif_off, n1, &data);
    write_exif_ifd(&f, e, date);
    if (gps) {
        ifd_open(&f, &w, gps_off, n2, &data);
        write_gps_ifd(&f, &e->gps);
    }
    if (data > w.len) {
        w.len = data;   /* the last value's pad byte, so the TIFF ends even */
    }

    total = APP1_HEAD_LEN + w.len;
    if (w.fail || (total > APP1_MAX_LEN)) {
        return 0U;
    }
    if (out != NULL) {
        out[0] = 0xFFU;
        out[1] = 0xE1U;
        out[2] = (uint8_t)((total - 2U) >> 8);
        out[3] = (uint8_t)((total - 2U) & 0xFFU);
        (void)memcpy(&out[4], EXIF_ID, sizeof(EXIF_ID));
    }
    return total;
}

size_t camera_drv_exif_size(const camera_drv_exif_t *e)
{
    if (e == NULL) {
        return 0U;
    }
    return layout(e, NULL, 0U);
}

size_t camera_drv_exif_build(const camera_drv_exif_t *e, uint8_t *out, size_t cap)
{
    size_t n;

    if ((e == NULL) || (out == NULL)) {
        return 0U;
    }
    n = layout(e, NULL, 0U);
    if ((n == 0U) || (n > cap)) {
        return 0U;
    }
    /* The padding bytes are never written one by one: the buffer starts clear. */
    (void)memset(out, 0, n);
    return layout(e, out, n);
}

/* ---- Where it goes ---------------------------------------------------------------- */

size_t camera_drv_exif_insert_at(const uint8_t *jpg, size_t len)
{
    size_t at = 2U;

    if ((jpg == NULL) || (len < 4U) || (jpg[0] != 0xFFU) || (jpg[1] != 0xD8U) ||
        (jpg[2] != 0xFFU)) {
        return 0U;
    }
    /* Walk the segments up to the image data: each is FF, a marker, a big-endian
     * length that counts itself. Stop at the scan, the end, or anything odd. */
    while (((at + 4U) <= len) && (jpg[at] == 0xFFU)) {
        uint8_t const marker = jpg[at + 1U];
        size_t const seg = ((size_t)jpg[at + 2U] << 8) | (size_t)jpg[at + 3U];

        if ((marker == 0xDAU) || (marker == 0xD9U) || (marker == 0x00U) || (marker == 0xFFU) ||
            ((marker >= 0xD0U) && (marker <= 0xD7U)) || (marker == 0x01U) || (seg < 2U)) {
            break;
        }
        if ((marker == 0xE1U) && (seg >= 8U) && ((at + 4U + sizeof(EXIF_ID)) <= len) &&
            (memcmp(&jpg[at + 4U], EXIF_ID, sizeof(EXIF_ID)) == 0)) {
            return 0U;
        }
        at += 2U + seg;
    }
    return 2U;
}
