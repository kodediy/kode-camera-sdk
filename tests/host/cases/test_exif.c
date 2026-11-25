/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_exif.c
 * @brief camera_drv_exif: the segment a photo gets is a segment a reader can read -
 *        its marker and length, its TIFF header, every directory sorted and
 *        typed, every value inside the segment at an even offset - and says
 *        what it was told: the date, the size, the exposure and the place,
 *        with no tag at all for what was not known. Bad input is refused,
 *        and a short buffer is left as it was.
 *
 * The cases carry a small TIFF reader of their own (a helper, not shipped):
 * it checks the structure a reader relies on before it looks at any value,
 * and it claims every byte it visits so two values can never overlap.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "camera_drv_exif.h"

/* ---- A TIFF reader ---------------------------------------------------------------- */

#define IMG_MAX  1024U
#define ENT_MAX  16U

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    size_t   at;     /* where the value's bytes are, in the TIFF          */
    size_t   bytes;
} ent_t;

typedef struct {
    size_t n;
    ent_t  e[ENT_MAX];
} dir_t;

typedef struct {
    uint8_t  seg[IMG_MAX];
    size_t   len;          /* the whole segment                         */
    const uint8_t *tiff;   /* the TIFF header inside it                 */
    size_t   tn;
    uint8_t  used[IMG_MAX];
    dir_t    ifd0;
    dir_t    exif;
    dir_t    gps;
    bool     has_gps;
} img_t;

enum { DIR_IFD0 = 0, DIR_EXIF = 1, DIR_GPS = 2 };

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(&p[2]) << 16);
}

static size_t type_size(uint16_t t)
{
    switch (t) {
    case 1U:  return 1U;   /* BYTE      */
    case 2U:  return 1U;   /* ASCII     */
    case 3U:  return 2U;   /* SHORT     */
    case 4U:  return 4U;   /* LONG      */
    case 5U:  return 8U;   /* RATIONAL  */
    case 7U:  return 1U;   /* UNDEFINED */
    default:  return 0U;
    }
}

typedef struct {
    int      dir;
    uint16_t tag;
    uint16_t type;
} tagtype_t;

/* Every tag the builder may write, and the type the standard gives it. */
static const tagtype_t TAGTYPES[] = {
    { DIR_IFD0, 0x010FU, 2U }, { DIR_IFD0, 0x0110U, 2U }, { DIR_IFD0, 0x0112U, 3U },
    { DIR_IFD0, 0x011AU, 5U }, { DIR_IFD0, 0x011BU, 5U }, { DIR_IFD0, 0x0128U, 3U },
    { DIR_IFD0, 0x0131U, 2U }, { DIR_IFD0, 0x0132U, 2U }, { DIR_IFD0, 0x8769U, 4U },
    { DIR_IFD0, 0x8825U, 4U },
    { DIR_EXIF, 0x829AU, 5U }, { DIR_EXIF, 0x8827U, 3U }, { DIR_EXIF, 0x9000U, 7U },
    { DIR_EXIF, 0x9003U, 2U }, { DIR_EXIF, 0x9004U, 2U }, { DIR_EXIF, 0x9010U, 2U },
    { DIR_EXIF, 0x9011U, 2U }, { DIR_EXIF, 0x9012U, 2U }, { DIR_EXIF, 0xA001U, 3U },
    { DIR_EXIF, 0xA002U, 4U }, { DIR_EXIF, 0xA003U, 4U },
    { DIR_GPS, 0x0000U, 1U }, { DIR_GPS, 0x0001U, 2U }, { DIR_GPS, 0x0002U, 5U },
    { DIR_GPS, 0x0003U, 2U }, { DIR_GPS, 0x0004U, 5U }, { DIR_GPS, 0x0005U, 1U },
    { DIR_GPS, 0x0006U, 5U }, { DIR_GPS, 0x0007U, 5U }, { DIR_GPS, 0x001DU, 2U },
};

static void claim(img_t *m, size_t at, size_t n)
{
    TEST_ASSERT_TRUE_MESSAGE((at + n) <= m->tn, "a structure runs past the segment");
    for (size_t i = at; i < (at + n); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(0, m->used[i], "two structures share a byte");
        m->used[i] = 1U;
    }
}

/* Read the directory at @p off, checking it as a reader would need it. */
static void read_dir(img_t *m, int which, size_t off, dir_t *d)
{
    size_t n;
    size_t end;

    TEST_ASSERT_EQUAL_MESSAGE(0U, off % 2U, "a directory at an odd offset");
    TEST_ASSERT_TRUE((off + 2U) <= m->tn);
    n = rd16(&m->tiff[off]);
    TEST_ASSERT_TRUE_MESSAGE((n > 0U) && (n <= ENT_MAX), "a directory with no entries or too many");
    end = off + 2U + (n * 12U) + 4U;
    claim(m, off, end - off);
    d->n = n;
    for (size_t i = 0U; i < n; i++) {
        const uint8_t *p = &m->tiff[off + 2U + (i * 12U)];
        ent_t *x = &d->e[i];
        bool known = false;

        x->tag = (uint16_t)rd16(p);
        x->type = (uint16_t)rd16(&p[2]);
        x->count = rd32(&p[4]);
        if (i > 0U) {
            TEST_ASSERT_TRUE_MESSAGE(x->tag > d->e[i - 1U].tag, "tags not sorted ascending");
        }
        for (size_t k = 0U; k < (sizeof(TAGTYPES) / sizeof(TAGTYPES[0])); k++) {
            if ((TAGTYPES[k].dir == which) && (TAGTYPES[k].tag == x->tag)) {
                known = true;
                TEST_ASSERT_EQUAL_UINT16(TAGTYPES[k].type, x->type);
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(known, "a tag the builder should not write");
        TEST_ASSERT_TRUE(x->count > 0U);
        x->bytes = type_size(x->type) * x->count;
        if (x->bytes <= 4U) {
            x->at = off + 2U + (i * 12U) + 8U;
            for (size_t b = x->bytes; b < 4U; b++) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, p[8U + b], "an inline value not padded with zeros");
            }
        } else {
            x->at = rd32(&p[8]);
            TEST_ASSERT_EQUAL_MESSAGE(0U, x->at % 2U, "a value at an odd offset");
            TEST_ASSERT_TRUE_MESSAGE(x->at >= end, "a value inside a directory");
            claim(m, x->at, x->bytes);
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, rd32(&m->tiff[end - 4U]), "a next-IFD offset that is not 0");
}

static const ent_t *get(const dir_t *d, uint16_t tag)
{
    for (size_t i = 0U; i < d->n; i++) {
        if (d->e[i].tag == tag) {
            return &d->e[i];
        }
    }
    return NULL;
}

static const ent_t *need(const dir_t *d, uint16_t tag)
{
    const ent_t *x = get(d, tag);
    TEST_ASSERT_NOT_NULL_MESSAGE(x, "a tag that should be there is not");
    return x;
}

static const char *str_of(const img_t *m, const dir_t *d, uint16_t tag)
{
    const ent_t *x = need(d, tag);
    const char *s = (const char *)&m->tiff[x->at];

    TEST_ASSERT_EQUAL_UINT16(2U, x->type);
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', s[x->count - 1U], "an ASCII value without its NUL");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->count - 1U, strlen(s), "an ASCII count that is not its length + 1");
    return s;
}

static uint32_t num_of(const img_t *m, const dir_t *d, uint16_t tag)
{
    const ent_t *x = need(d, tag);

    TEST_ASSERT_EQUAL_UINT32(1U, x->count);
    return (x->type == 3U) ? rd16(&m->tiff[x->at]) : rd32(&m->tiff[x->at]);
}

static void rat_of(const img_t *m, const dir_t *d, uint16_t tag, size_t i, uint32_t *num,
                   uint32_t *den)
{
    const ent_t *x = need(d, tag);

    TEST_ASSERT_EQUAL_UINT16(5U, x->type);
    TEST_ASSERT_TRUE(i < x->count);
    *num = rd32(&m->tiff[x->at + (i * 8U)]);
    *den = rd32(&m->tiff[x->at + (i * 8U) + 4U]);
}

/* Parse the segment in @p m: header, then every directory it points to. */
static void parse(img_t *m)
{
    const uint8_t *s = m->seg;

    TEST_ASSERT_EQUAL_UINT8(0xFFU, s[0]);
    TEST_ASSERT_EQUAL_UINT8(0xE1U, s[1]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(m->len - 2U), ((uint32_t)s[2] << 8) | s[3],
                                     "the length field is not the segment minus its marker");
    TEST_ASSERT_TRUE(m->len < 65536U);
    TEST_ASSERT_EQUAL_MEMORY("Exif\0\0", &s[4], 6U);
    m->tiff = &s[10];
    m->tn = m->len - 10U;
    TEST_ASSERT_EQUAL_MEMORY("II", m->tiff, 2U);
    TEST_ASSERT_EQUAL_UINT32(42U, rd16(&m->tiff[2]));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8U, rd32(&m->tiff[4]), "IFD0 not right after the header");
    memset(m->used, 0, sizeof(m->used));
    claim(m, 0U, 8U);

    read_dir(m, DIR_IFD0, 8U, &m->ifd0);
    read_dir(m, DIR_EXIF, num_of(m, &m->ifd0, 0x8769U), &m->exif);
    m->has_gps = (get(&m->ifd0, 0x8825U) != NULL);
    if (m->has_gps) {
        read_dir(m, DIR_GPS, num_of(m, &m->ifd0, 0x8825U), &m->gps);
    }
    /* No hole but the pad byte after an odd-sized value. */
    for (size_t i = 0U; i < m->tn; i++) {
        if (m->used[i] == 0U) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, m->tiff[i], "an unclaimed byte that is not zero");
            TEST_ASSERT_TRUE_MESSAGE((i > 0U) && (m->used[i - 1U] != 0U), "a hole of 2+ bytes");
        }
    }
}

static void build(img_t *m, const camera_drv_exif_t *e)
{
    memset(m, 0, sizeof(*m));
    m->len = camera_drv_exif_build(e, m->seg, sizeof(m->seg));
    TEST_ASSERT_TRUE_MESSAGE(m->len > 0U, "camera_drv_exif_build refused good input");
    parse(m);
}

/* ---- Inputs ------------------------------------------------------------------------ */

/* What the camera knows without a fix: a photo of 1600 x 1200 on 2026-10-19. */
static camera_drv_exif_t base(void)
{
    camera_drv_exif_t e;

    memset(&e, 0, sizeof(e));
    e.width = 1600U;
    e.height = 1200U;
    e.orientation = 1U;
    e.taken.year = 2026U;
    e.taken.month = 10U;
    e.taken.day = 19U;
    e.taken.hour = 14U;
    e.taken.minute = 5U;
    e.taken.second = 9U;
    e.exposure_us = 10000U;
    e.iso = 100U;
    return e;
}

/* Madrid, 667.25 m, at 12:30:45 UTC on 2026-10-19. */
static camera_drv_exif_t with_fix(void)
{
    camera_drv_exif_t e = base();

    e.gps.valid = true;
    e.gps.lat_e7 = 404168000;
    e.gps.lon_e7 = -37038000;
    e.gps.alt_mm = 667250;
    e.gps.alt_valid = true;
    e.gps.utc.year = 2026U;
    e.gps.utc.month = 10U;
    e.gps.utc.day = 19U;
    e.gps.utc.hour = 12U;
    e.gps.utc.minute = 30U;
    e.gps.utc.second = 45U;
    return e;
}

static void expect_rat(const img_t *m, const dir_t *d, uint16_t tag, size_t i, uint32_t n, uint32_t den)
{
    uint32_t gn;
    uint32_t gd;

    rat_of(m, d, tag, i, &gn, &gd);
    TEST_ASSERT_EQUAL_UINT32(n, gn);
    TEST_ASSERT_EQUAL_UINT32(den, gd);
}

/* ---- The segment ------------------------------------------------------------------- */

TEST_CASE("camera_drv_exif: the segment has its marker, its length and the Exif header", "[exif]")
{
    camera_drv_exif_t const e = base();
    img_t m;

    build(&m, &e);   /* parse() checks 0xFF 0xE1, the length, "Exif\0\0", "II*\0" and IFD0 at 8 */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, m.len % 2U, "the segment ends on an odd byte");
}

TEST_CASE("camera_drv_exif: every directory is sorted, typed and its values sit inside, even, apart", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    camera_drv_exif_t f = with_fix();
    img_t m;

    f.software = "Kode camera SDK 1.0";
    build(&m, &e);   /* read_dir() holds every directory to the rules */
    TEST_ASSERT_TRUE(m.has_gps);
    build(&m, &f);
    TEST_ASSERT_TRUE(m.has_gps);
}

TEST_CASE("camera_drv_exif: the same input builds the same bytes", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    uint8_t a[IMG_MAX];
    uint8_t b[IMG_MAX];
    size_t const na = camera_drv_exif_build(&e, a, sizeof(a));
    size_t const nb = camera_drv_exif_build(&e, b, 700U);

    TEST_ASSERT_TRUE(na > 0U);
    TEST_ASSERT_EQUAL_UINT32(na, nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, na);
}

/* ---- IFD0 -------------------------------------------------------------------------- */

TEST_CASE("camera_drv_exif: IFD0 without a fix holds exactly the camera's tags", "[exif]")
{
    camera_drv_exif_t const e = base();
    uint16_t const want[] = { 0x010FU, 0x0110U, 0x0112U, 0x011AU, 0x011BU, 0x0128U, 0x0132U, 0x8769U };
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want) / sizeof(want[0]), m.ifd0.n);
    for (size_t i = 0U; i < m.ifd0.n; i++) {
        TEST_ASSERT_EQUAL_UINT16(want[i], m.ifd0.e[i].tag);
    }
}

TEST_CASE("camera_drv_exif: Make and Model default to Kode and Kode Dot Camera", "[exif]")
{
    camera_drv_exif_t const e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("Kode", str_of(&m, &m.ifd0, 0x010FU));
    TEST_ASSERT_EQUAL_STRING("Kode Dot Camera", str_of(&m, &m.ifd0, 0x0110U));
}

TEST_CASE("camera_drv_exif: Make, Model and Software are the ones given, and Software is optional", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.ifd0, 0x0131U));
    e.make = "Acme";
    e.model = "X1";
    e.software = "camera-sdk 0.3";
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("Acme", str_of(&m, &m.ifd0, 0x010FU));
    TEST_ASSERT_EQUAL_STRING("X1", str_of(&m, &m.ifd0, 0x0110U));
    TEST_ASSERT_EQUAL_STRING("camera-sdk 0.3", str_of(&m, &m.ifd0, 0x0131U));
}

TEST_CASE("camera_drv_exif: a string is cut at 63 characters and a stray byte becomes a question mark", "[exif]")
{
    char s63[64];
    char s70[71];
    camera_drv_exif_t e = base();
    img_t m;

    memset(s63, 'a', 63U);
    s63[63] = '\0';
    memset(s70, 'b', 70U);
    s70[70] = '\0';
    e.make = s63;
    e.model = s70;
    e.software = "caf\xC3\xA9\x01";
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING(s63, str_of(&m, &m.ifd0, 0x010FU));
    TEST_ASSERT_EQUAL_UINT32(64U, need(&m.ifd0, 0x0110U)->count);
    TEST_ASSERT_EQUAL_UINT32(63U, strlen(str_of(&m, &m.ifd0, 0x0110U)));
    TEST_ASSERT_EQUAL_CHAR('b', str_of(&m, &m.ifd0, 0x0110U)[62]);
    TEST_ASSERT_EQUAL_STRING("caf???", str_of(&m, &m.ifd0, 0x0131U));
}

TEST_CASE("camera_drv_exif: DateTime is the local time, 20 bytes with its NUL", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(20U, need(&m.ifd0, 0x0132U)->count);
    TEST_ASSERT_EQUAL_STRING("2026:10:19 14:05:09", str_of(&m, &m.ifd0, 0x0132U));
    e.taken.year = 1999U;
    e.taken.month = 12U;
    e.taken.day = 31U;
    e.taken.hour = 23U;
    e.taken.minute = 59U;
    e.taken.second = 59U;
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("1999:12:31 23:59:59", str_of(&m, &m.ifd0, 0x0132U));
}

TEST_CASE("camera_drv_exif: every orientation 1 to 8 is written as given", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    for (uint8_t o = 1U; o <= 8U; o++) {
        e.orientation = o;
        build(&m, &e);
        TEST_ASSERT_EQUAL_UINT32(o, num_of(&m, &m.ifd0, 0x0112U));
    }
}

TEST_CASE("camera_drv_exif: resolution is 72/1 by 72/1 in inches", "[exif]")
{
    camera_drv_exif_t const e = base();
    img_t m;

    build(&m, &e);
    expect_rat(&m, &m.ifd0, 0x011AU, 0U, 72U, 1U);
    expect_rat(&m, &m.ifd0, 0x011BU, 0U, 72U, 1U);
    TEST_ASSERT_EQUAL_UINT32(2U, num_of(&m, &m.ifd0, 0x0128U));
}

/* ---- Exif IFD ---------------------------------------------------------------------- */

TEST_CASE("camera_drv_exif: the Exif IFD says version 0232, sRGB and the pixel size", "[exif]")
{
    camera_drv_exif_t const e = base();
    img_t m;
    const ent_t *v;

    build(&m, &e);
    v = need(&m.exif, 0x9000U);
    TEST_ASSERT_EQUAL_UINT32(4U, v->count);
    TEST_ASSERT_EQUAL_MEMORY("0232", &m.tiff[v->at], 4U);
    TEST_ASSERT_EQUAL_UINT32(1U, num_of(&m, &m.exif, 0xA001U));
    TEST_ASSERT_EQUAL_UINT16(4U, need(&m.exif, 0xA002U)->type);
    TEST_ASSERT_EQUAL_UINT32(1600U, num_of(&m, &m.exif, 0xA002U));
    TEST_ASSERT_EQUAL_UINT32(1200U, num_of(&m, &m.exif, 0xA003U));
}

TEST_CASE("camera_drv_exif: a size of 65535 by 1 survives and a size of 0 leaves both tags out", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    e.width = 65535U;
    e.height = 1U;
    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(65535U, num_of(&m, &m.exif, 0xA002U));
    TEST_ASSERT_EQUAL_UINT32(1U, num_of(&m, &m.exif, 0xA003U));
    e.height = 0U;
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.exif, 0xA002U));
    TEST_ASSERT_NULL(get(&m.exif, 0xA003U));
}

TEST_CASE("camera_drv_exif: DateTimeOriginal and DateTimeDigitized match DateTime", "[exif]")
{
    camera_drv_exif_t const e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("2026:10:19 14:05:09", str_of(&m, &m.exif, 0x9003U));
    TEST_ASSERT_EQUAL_STRING("2026:10:19 14:05:09", str_of(&m, &m.exif, 0x9004U));
}

TEST_CASE("camera_drv_exif: the three OffsetTime tags say how far the dates are from UTC", "[exif]")
{
    struct { int16_t min; const char *s; } const t[] = {
        { 60, "+01:00" }, { 0, "+00:00" }, { -570, "-09:30" }, { 840, "+14:00" },
        { -720, "-12:00" }, { 345, "+05:45" },
    };
    camera_drv_exif_t e = base();
    img_t m;

    e.has_offset = true;
    for (size_t i = 0U; i < (sizeof(t) / sizeof(t[0])); i++) {
        e.offset_min = t[i].min;
        build(&m, &e);
        TEST_ASSERT_EQUAL_STRING(t[i].s, str_of(&m, &m.exif, 0x9010U));
        TEST_ASSERT_EQUAL_STRING(t[i].s, str_of(&m, &m.exif, 0x9011U));
        TEST_ASSERT_EQUAL_STRING(t[i].s, str_of(&m, &m.exif, 0x9012U));
    }
}

TEST_CASE("camera_drv_exif: no offset, or no date, leaves the OffsetTime tags out", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.exif, 0x9010U));
    TEST_ASSERT_NULL(get(&m.exif, 0x9011U));
    TEST_ASSERT_NULL(get(&m.exif, 0x9012U));
    e.has_offset = true;
    e.offset_min = 9999;   /* not looked at: there is no date */
    e.taken.year = 0U;
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.exif, 0x9011U));
}

TEST_CASE("camera_drv_exif: an offset past -12:00..+14:00 is refused", "[exif]")
{
    uint8_t buf[CAMERA_DRV_EXIF_MAX_LENGTH];
    camera_drv_exif_t e = base();

    e.has_offset = true;
    e.offset_min = 841;
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_size(&e));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(&e, buf, sizeof(buf)));
    e.offset_min = -721;
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_size(&e));
}

TEST_CASE("camera_drv_exif: exposure is the exact fraction of a second, reduced", "[exif]")
{
    struct { uint32_t us; uint32_t n; uint32_t d; } const t[] = {
        { 10000U, 1U, 100U },       /* 1/100 s       */
        { 2000000U, 2U, 1U },       /* 2 s           */
        { 1000000U, 1U, 1U },       /* 1 s           */
        { 12500U, 1U, 80U },        /* 1/80 s        */
        { 1500000U, 3U, 2U },       /* 1.5 s         */
        { 12345U, 2469U, 200000U }, /* no 1/N: exact */
        { 1U, 1U, 1000000U },       /* the shortest  */
    };
    camera_drv_exif_t e = base();
    img_t m;

    for (size_t i = 0U; i < (sizeof(t) / sizeof(t[0])); i++) {
        e.exposure_us = t[i].us;
        build(&m, &e);
        expect_rat(&m, &m.exif, 0x829AU, 0U, t[i].n, t[i].d);
    }
}

TEST_CASE("camera_drv_exif: an exposure or an ISO of 0 leaves its tag out", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(100U, num_of(&m, &m.exif, 0x8827U));
    e.exposure_us = 0U;
    e.iso = 0U;
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.exif, 0x829AU));
    TEST_ASSERT_NULL(get(&m.exif, 0x8827U));
}

TEST_CASE("camera_drv_exif: year 0 leaves every date tag out and still builds", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    e.taken.year = 0U;
    e.taken.month = 99U;   /* not looked at: the date is unknown */
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.ifd0, 0x0132U));
    TEST_ASSERT_NULL(get(&m.exif, 0x9003U));
    TEST_ASSERT_NULL(get(&m.exif, 0x9004U));
    TEST_ASSERT_NOT_NULL(get(&m.exif, 0x829AU));
}

/* ---- GPS --------------------------------------------------------------------------- */

TEST_CASE("camera_drv_exif: without a fix there is no GPS IFD and no pointer to one", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    camera_drv_exif_t const g = with_fix();
    img_t m;
    img_t n;

    e.gps.valid = false;
    e.gps.lat_e7 = 2000000000;   /* off the globe, but not looked at */
    build(&m, &e);
    TEST_ASSERT_FALSE(m.has_gps);
    TEST_ASSERT_NULL(get(&m.ifd0, 0x8825U));
    build(&n, &g);
    TEST_ASSERT_TRUE(n.len > m.len);
}

TEST_CASE("camera_drv_exif: the GPS IFD names its version, its tags and its references", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    uint16_t const want[] = { 0x0000U, 0x0001U, 0x0002U, 0x0003U, 0x0004U, 0x0005U, 0x0006U,
                              0x0007U, 0x001DU };
    img_t m;
    const ent_t *v;

    build(&m, &e);
    TEST_ASSERT_TRUE(m.has_gps);
    TEST_ASSERT_EQUAL_UINT32(sizeof(want) / sizeof(want[0]), m.gps.n);
    for (size_t i = 0U; i < m.gps.n; i++) {
        TEST_ASSERT_EQUAL_UINT16(want[i], m.gps.e[i].tag);
    }
    v = need(&m.gps, 0x0000U);
    TEST_ASSERT_EQUAL_UINT32(4U, v->count);
    TEST_ASSERT_EQUAL_UINT8(2U, m.tiff[v->at]);
    TEST_ASSERT_EQUAL_UINT8(3U, m.tiff[v->at + 1U]);
    TEST_ASSERT_EQUAL_UINT8(0U, m.tiff[v->at + 2U]);
    TEST_ASSERT_EQUAL_UINT8(0U, m.tiff[v->at + 3U]);
    TEST_ASSERT_EQUAL_STRING("N", str_of(&m, &m.gps, 0x0001U));
    TEST_ASSERT_EQUAL_STRING("W", str_of(&m, &m.gps, 0x0003U));
    TEST_ASSERT_EQUAL_UINT32(2U, need(&m.gps, 0x0001U)->count);
}

TEST_CASE("camera_drv_exif: 40.4168 N 3.7038 W is 40 25 0.48 and 3 42 13.68", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    img_t m;
    uint32_t n;
    uint32_t d;

    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(3U, need(&m.gps, 0x0002U)->count);
    expect_rat(&m, &m.gps, 0x0002U, 0U, 40U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 1U, 25U, 1U);
    rat_of(&m, &m.gps, 0x0002U, 2U, &n, &d);
    TEST_ASSERT_TRUE(d >= 1000U);
    /* 0.48 s to 1/1000 s: |n/d - 0.48| < 0.001  <=>  |n*1000 - 480*d| < d */
    TEST_ASSERT_TRUE(((uint64_t)n * 1000U) < ((uint64_t)480U * d) + d);
    TEST_ASSERT_TRUE(((uint64_t)n * 1000U) + d > ((uint64_t)480U * d));
    expect_rat(&m, &m.gps, 0x0004U, 0U, 3U, 1U);
    expect_rat(&m, &m.gps, 0x0004U, 1U, 42U, 1U);
    rat_of(&m, &m.gps, 0x0004U, 2U, &n, &d);
    TEST_ASSERT_TRUE(((uint64_t)n * 1000U) < ((uint64_t)13680U * d) + d);
    TEST_ASSERT_TRUE(((uint64_t)n * 1000U) + d > ((uint64_t)13680U * d));
}

TEST_CASE("camera_drv_exif: the seconds are exact over 1000000", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    img_t m;

    build(&m, &e);
    expect_rat(&m, &m.gps, 0x0002U, 2U, 480000U, 1000000U);
    expect_rat(&m, &m.gps, 0x0004U, 2U, 13680000U, 1000000U);
}

TEST_CASE("camera_drv_exif: south and east are S and E, with their own degrees", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.lat_e7 = -338688000;   /* 33 52 7.68 S  */
    e.gps.lon_e7 = 1512093000;   /* 151 12 33.48 E */
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("S", str_of(&m, &m.gps, 0x0001U));
    TEST_ASSERT_EQUAL_STRING("E", str_of(&m, &m.gps, 0x0003U));
    expect_rat(&m, &m.gps, 0x0002U, 0U, 33U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 1U, 52U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 2U, 7680000U, 1000000U);
    expect_rat(&m, &m.gps, 0x0004U, 0U, 151U, 1U);
    expect_rat(&m, &m.gps, 0x0004U, 1U, 12U, 1U);
    expect_rat(&m, &m.gps, 0x0004U, 2U, 33480000U, 1000000U);
}

TEST_CASE("camera_drv_exif: the poles, the dateline and the origin are in range", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.lat_e7 = 900000000;
    e.gps.lon_e7 = -1800000000;
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("N", str_of(&m, &m.gps, 0x0001U));
    expect_rat(&m, &m.gps, 0x0002U, 0U, 90U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 2U, 0U, 1000000U);
    expect_rat(&m, &m.gps, 0x0004U, 0U, 180U, 1U);
    e.gps.lat_e7 = 0;
    e.gps.lon_e7 = 0;
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("N", str_of(&m, &m.gps, 0x0001U));
    TEST_ASSERT_EQUAL_STRING("E", str_of(&m, &m.gps, 0x0003U));
    expect_rat(&m, &m.gps, 0x0002U, 0U, 0U, 1U);
}

TEST_CASE("camera_drv_exif: the smallest step, 1e-7 degree, is 0.00036 seconds of arc", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.lat_e7 = 1;
    e.gps.lon_e7 = -9999999;   /* 0 59 59.99964 W */
    build(&m, &e);
    expect_rat(&m, &m.gps, 0x0002U, 0U, 0U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 1U, 0U, 1U);
    expect_rat(&m, &m.gps, 0x0002U, 2U, 360U, 1000000U);
    expect_rat(&m, &m.gps, 0x0004U, 0U, 0U, 1U);
    expect_rat(&m, &m.gps, 0x0004U, 1U, 59U, 1U);
    expect_rat(&m, &m.gps, 0x0004U, 2U, 59999640U, 1000000U);
}

TEST_CASE("camera_drv_exif: altitude above the sea is AltRef 0 over 1000", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    img_t m;
    const ent_t *r;

    build(&m, &e);
    r = need(&m.gps, 0x0005U);
    TEST_ASSERT_EQUAL_UINT32(1U, r->count);
    TEST_ASSERT_EQUAL_UINT8(0U, m.tiff[r->at]);
    expect_rat(&m, &m.gps, 0x0006U, 0U, 667250U, 1000U);
}

TEST_CASE("camera_drv_exif: altitude below the sea is AltRef 1 and its magnitude", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.alt_mm = -12500;
    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT8(1U, m.tiff[need(&m.gps, 0x0005U)->at]);
    expect_rat(&m, &m.gps, 0x0006U, 0U, 12500U, 1000U);
    e.gps.alt_mm = INT32_MIN;
    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT8(1U, m.tiff[need(&m.gps, 0x0005U)->at]);
    expect_rat(&m, &m.gps, 0x0006U, 0U, 2147483648U, 1000U);
}

TEST_CASE("camera_drv_exif: without alt_valid there are no altitude tags", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.alt_valid = false;
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.gps, 0x0005U));
    TEST_ASSERT_NULL(get(&m.gps, 0x0006U));
    TEST_ASSERT_NOT_NULL(get(&m.gps, 0x0002U));
}

TEST_CASE("camera_drv_exif: GPSTimeStamp and GPSDateStamp are the UTC time of the fix", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_EQUAL_UINT32(3U, need(&m.gps, 0x0007U)->count);
    expect_rat(&m, &m.gps, 0x0007U, 0U, 12U, 1U);
    expect_rat(&m, &m.gps, 0x0007U, 1U, 30U, 1U);
    expect_rat(&m, &m.gps, 0x0007U, 2U, 45U, 1U);
    TEST_ASSERT_EQUAL_UINT32(11U, need(&m.gps, 0x001DU)->count);
    TEST_ASSERT_EQUAL_STRING("2026:10:19", str_of(&m, &m.gps, 0x001DU));
}

TEST_CASE("camera_drv_exif: a fix with UTC year 0 has no time or date stamp", "[exif]")
{
    camera_drv_exif_t e = with_fix();
    img_t m;

    e.gps.utc.year = 0U;
    build(&m, &e);
    TEST_ASSERT_NULL(get(&m.gps, 0x0007U));
    TEST_ASSERT_NULL(get(&m.gps, 0x001DU));
    TEST_ASSERT_NOT_NULL(get(&m.gps, 0x0002U));
}

TEST_CASE("camera_drv_exif: the GPS pointer is the offset of a directory inside the segment", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    img_t m;

    build(&m, &e);
    TEST_ASSERT_TRUE(num_of(&m, &m.ifd0, 0x8825U) > num_of(&m, &m.ifd0, 0x8769U));
    TEST_ASSERT_TRUE(num_of(&m, &m.ifd0, 0x8825U) < m.tn);
}

/* ---- Refusals ---------------------------------------------------------------------- */

static void refuse(const camera_drv_exif_t *e, const char *why)
{
    uint8_t buf[IMG_MAX];

    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, camera_drv_exif_build(e, buf, sizeof(buf)), why);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, camera_drv_exif_size(e), why);
    for (size_t i = 0U; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xA5U, buf[i], "a refusal wrote something");
    }
}

TEST_CASE("camera_drv_exif: an orientation outside 1 to 8 is refused", "[exif]")
{
    camera_drv_exif_t e = base();

    e.orientation = 0U;
    refuse(&e, "orientation 0");
    e.orientation = 9U;
    refuse(&e, "orientation 9");
    e.orientation = 255U;
    refuse(&e, "orientation 255");
}

TEST_CASE("camera_drv_exif: a date that does not exist is refused", "[exif]")
{
    camera_drv_exif_t e = base();

    e.taken.month = 13U;
    refuse(&e, "month 13");
    e = base();
    e.taken.month = 0U;
    refuse(&e, "month 0");
    e = base();
    e.taken.day = 0U;
    refuse(&e, "day 0");
    e = base();
    e.taken.month = 4U;
    e.taken.day = 31U;
    refuse(&e, "31 April");
    e = base();
    e.taken.year = 2025U;
    e.taken.month = 2U;
    e.taken.day = 29U;
    refuse(&e, "29 February 2025");
    e = base();
    e.taken.year = 1900U;
    e.taken.month = 2U;
    e.taken.day = 29U;
    refuse(&e, "29 February 1900");
    e = base();
    e.taken.hour = 24U;
    refuse(&e, "hour 24");
    e = base();
    e.taken.minute = 60U;
    refuse(&e, "minute 60");
    e = base();
    e.taken.second = 60U;
    refuse(&e, "second 60");
    e = base();
    e.taken.year = 10000U;
    refuse(&e, "year 10000");
}

TEST_CASE("camera_drv_exif: 29 February of a leap year is a date", "[exif]")
{
    camera_drv_exif_t e = base();
    img_t m;

    e.taken.year = 2024U;
    e.taken.month = 2U;
    e.taken.day = 29U;
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("2024:02:29 14:05:09", str_of(&m, &m.ifd0, 0x0132U));
    e.taken.year = 2000U;   /* divisible by 400 */
    build(&m, &e);
    TEST_ASSERT_EQUAL_STRING("2000:02:29 14:05:09", str_of(&m, &m.ifd0, 0x0132U));
}

TEST_CASE("camera_drv_exif: a position off the globe or a bad UTC time is refused", "[exif]")
{
    camera_drv_exif_t e = with_fix();

    e.gps.lat_e7 = 900000001;
    refuse(&e, "lat past 90");
    e = with_fix();
    e.gps.lat_e7 = -900000001;
    refuse(&e, "lat past -90");
    e = with_fix();
    e.gps.lon_e7 = 1800000001;
    refuse(&e, "lon past 180");
    e = with_fix();
    e.gps.lon_e7 = INT32_MIN;
    refuse(&e, "lon at INT32_MIN");
    e = with_fix();
    e.gps.utc.month = 13U;
    refuse(&e, "UTC month 13");
    e = with_fix();
    e.gps.utc.hour = 24U;
    refuse(&e, "UTC hour 24");
}

TEST_CASE("camera_drv_exif: a NULL input or output builds nothing", "[exif]")
{
    camera_drv_exif_t const e = base();
    uint8_t buf[IMG_MAX];

    memset(buf, 0xA5, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(&e, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_size(NULL));
    TEST_ASSERT_EQUAL_UINT8(0xA5U, buf[0]);
}

TEST_CASE("camera_drv_exif: a buffer one byte short is refused and left untouched", "[exif]")
{
    camera_drv_exif_t const full = with_fix();
    camera_drv_exif_t const bare = base();
    camera_drv_exif_t const es[2] = { full, bare };

    for (size_t k = 0U; k < 2U; k++) {
        uint8_t buf[IMG_MAX];
        size_t const n = camera_drv_exif_size(&es[k]);

        TEST_ASSERT_TRUE(n > 0U);
        memset(buf, 0xA5, sizeof(buf));
        TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(&es[k], buf, n - 1U));
        TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(&es[k], buf, 0U));
        TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_build(&es[k], buf, 1U));
        for (size_t i = 0U; i < sizeof(buf); i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xA5U, buf[i], "a short buffer was written");
        }
        TEST_ASSERT_EQUAL_UINT32(n, camera_drv_exif_build(&es[k], buf, n));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xA5U, buf[n], "wrote past the segment");
    }
}

/* ---- Size -------------------------------------------------------------------------- */

TEST_CASE("camera_drv_exif: camera_drv_exif_size is at least, and is, the length built", "[exif]")
{
    camera_drv_exif_t all[3];
    uint8_t buf[IMG_MAX];

    all[0] = base();
    all[0].software = "x";
    all[1] = with_fix();
    all[1].make = "Some make";
    all[2] = base();
    all[2].taken.year = 0U;
    all[2].exposure_us = 0U;
    all[2].iso = 0U;
    all[2].width = 0U;
    for (size_t i = 0U; i < 3U; i++) {
        size_t const sz = camera_drv_exif_size(&all[i]);
        size_t const n = camera_drv_exif_build(&all[i], buf, sizeof(buf));

        TEST_ASSERT_TRUE(n > 0U);
        TEST_ASSERT_TRUE(sz >= n);
        TEST_ASSERT_EQUAL_UINT32(n, sz);
    }
}

TEST_CASE("camera_drv_exif: the worst case fits CAMERA_DRV_EXIF_MAX_LENGTH, far under 64 KB", "[exif]")
{
    char s[100];
    camera_drv_exif_t e = with_fix();
    size_t n;

    e.has_offset = true;
    e.offset_min = -570;
    memset(s, 'z', sizeof(s) - 1U);
    s[sizeof(s) - 1U] = '\0';
    e.make = s;
    e.model = s;
    e.software = s;
    n = camera_drv_exif_size(&e);
    TEST_ASSERT_TRUE(n > 0U);
    TEST_ASSERT_TRUE_MESSAGE(n <= CAMERA_DRV_EXIF_MAX_LENGTH, "CAMERA_DRV_EXIF_MAX_LENGTH is too small");
    TEST_ASSERT_TRUE(n < 65536U);
    TEST_ASSERT_TRUE_MESSAGE((CAMERA_DRV_EXIF_MAX_LENGTH - n) < 64U, "CAMERA_DRV_EXIF_MAX_LENGTH is loose");
}

/* ---- Where it goes ----------------------------------------------------------------- */

static const uint8_t MIN_JPG[] = {
    0xFFU, 0xD8U,                                   /* SOI                    */
    0xFFU, 0xDBU, 0x00U, 0x04U, 0x00U, 0x10U,       /* DQT (shortened)        */
    0xFFU, 0xDAU, 0x00U, 0x02U, 0x12U, 0x34U,       /* SOS, then its data     */
    0xFFU, 0xD9U,                                   /* EOI                    */
};

TEST_CASE("camera_drv_exif: a JPEG gets its APP1 right after the SOI", "[exif]")
{
    TEST_ASSERT_EQUAL_UINT32(2U, camera_drv_exif_insert_at(MIN_JPG, sizeof(MIN_JPG)));
}

TEST_CASE("camera_drv_exif: a buffer that is not a JPEG gets nothing", "[exif]")
{
    uint8_t const png[] = { 0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU };
    uint8_t const half[] = { 0xFFU };
    uint8_t const bare[] = { 0xFFU, 0xD8U };
    uint8_t const bad[] = { 0xFFU, 0xD8U, 0x12U, 0x34U };
    uint8_t const rev[] = { 0xD8U, 0xFFU, 0xFFU, 0xDBU };

    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(png, sizeof(png)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(half, sizeof(half)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(bare, sizeof(bare)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(bad, sizeof(bad)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(rev, sizeof(rev)));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(MIN_JPG, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(NULL, 100U));
}

TEST_CASE("camera_drv_exif: a JPEG that already has an Exif APP1 gets no second one", "[exif]")
{
    camera_drv_exif_t const e = base();
    uint8_t jpg[IMG_MAX + 8U];
    size_t n = 0U;

    jpg[n++] = 0xFFU;
    jpg[n++] = 0xD8U;
    n += camera_drv_exif_build(&e, &jpg[n], IMG_MAX);
    TEST_ASSERT_TRUE(n > 2U);
    jpg[n++] = 0xFFU;
    jpg[n++] = 0xD9U;
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(jpg, n));
}

TEST_CASE("camera_drv_exif: an Exif APP1 behind a JFIF APP0 is found too", "[exif]")
{
    camera_drv_exif_t const e = base();
    uint8_t jpg[IMG_MAX + 32U];
    uint8_t const jfif[] = { 0xFFU, 0xE0U, 0x00U, 0x10U, 'J', 'F', 'I', 'F', 0U, 1U, 1U,
                             0U, 0U, 1U, 0U, 1U, 0U, 0U };
    size_t n = 0U;

    jpg[n++] = 0xFFU;
    jpg[n++] = 0xD8U;
    memcpy(&jpg[n], jfif, sizeof(jfif));
    n += sizeof(jfif);
    TEST_ASSERT_EQUAL_UINT32(2U, camera_drv_exif_insert_at(jpg, n));   /* JFIF alone: add */
    n += camera_drv_exif_build(&e, &jpg[n], IMG_MAX);
    jpg[n++] = 0xFFU;
    jpg[n++] = 0xD9U;
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(jpg, n));
}

TEST_CASE("camera_drv_exif: an APP1 that is not Exif, and a cut-off segment, do not stop an insert", "[exif]")
{
    uint8_t const xmp[] = { 0xFFU, 0xD8U, 0xFFU, 0xE1U, 0x00U, 0x0CU, 'h', 't', 't', 'p', ':', '/',
                            '/', 'n', 0xFFU, 0xD9U };
    uint8_t const cut[] = { 0xFFU, 0xD8U, 0xFFU, 0xE1U, 0x00U, 0x08U, 'E', 'x', 'i' };
    uint8_t const huge[] = { 0xFFU, 0xD8U, 0xFFU, 0xDBU, 0xFFU, 0xF0U, 0x00U };

    TEST_ASSERT_EQUAL_UINT32(2U, camera_drv_exif_insert_at(xmp, sizeof(xmp)));
    TEST_ASSERT_EQUAL_UINT32(2U, camera_drv_exif_insert_at(cut, sizeof(cut)));
    TEST_ASSERT_EQUAL_UINT32(2U, camera_drv_exif_insert_at(huge, sizeof(huge)));
}

TEST_CASE("camera_drv_exif: the JPEG, the APP1 and the rest splice into a JPEG that still reads", "[exif]")
{
    camera_drv_exif_t const e = with_fix();
    uint8_t out[IMG_MAX + sizeof(MIN_JPG)];
    size_t const k = camera_drv_exif_insert_at(MIN_JPG, sizeof(MIN_JPG));
    size_t n;
    size_t pos = 0U;

    TEST_ASSERT_EQUAL_UINT32(2U, k);
    memcpy(&out[pos], MIN_JPG, k);
    pos += k;
    n = camera_drv_exif_build(&e, &out[pos], IMG_MAX);
    TEST_ASSERT_TRUE(n > 0U);
    pos += n;
    memcpy(&out[pos], &MIN_JPG[k], sizeof(MIN_JPG) - k);
    pos += sizeof(MIN_JPG) - k;
    TEST_ASSERT_EQUAL_UINT8(0xD8U, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xE1U, out[3]);
    TEST_ASSERT_EQUAL_UINT8(0xFFU, out[2 + n]);   /* the DQT follows the APP1 */
    TEST_ASSERT_EQUAL_UINT8(0xDBU, out[2 + n + 1U]);
    TEST_ASSERT_EQUAL_UINT32(0U, camera_drv_exif_insert_at(out, pos));   /* now it has one */
}
