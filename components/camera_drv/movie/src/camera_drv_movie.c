/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_movie.c
 * @brief The .mov muxer: the journal's format, the writing and the building of
 *        the moov. See camera_drv_movie.h for the contract and how to use it.
 *
 * Three details, each with a case in tests/host/cases/test_movie.c: the header
 * reader walks the boxes with a stack, not by recursing; a failed open leaves its
 * descriptors at -1, so camera_drv_movie_abort() never closes them twice; and the
 * header reader's scratch buffer starts zeroed, so a box shorter than its
 * version says reads as zeros and not as stack garbage.
 *
 * THE JOURNAL (.VID_x.idx), little-endian.
 *
 * The header is 64 bytes, written and fsync'ed when the file is opened:
 *
 *      0  "KMOVIDX1"                       8  u32 version (1)
 *     12  u16 width, u16 height           16  u16 fps, u16 flags (bit 0 = audio)
 *     20  u32 unix_time                   24  u64 offset of the wide+mdat header in the .mov
 *     32  u16 rotation (0/90/180/270)     34..59 zeros
 *     60  u32 crc32 of bytes 0..59
 *
 * One record per block (36 + 8 * n bytes):
 *
 *      0  u32 "BLQ1"                       4  u32 n = frames in the block (<= CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX)
 *      8  u32 audio samples of the block  12  u32 block number
 *     16  u64 offset of the first frame   24  u64 offset of the audio chunk
 *     32  n x { u32 bytes of the JPEG, u32 empty slots right before this frame }
 *     32+8n  u32 crc32 of everything above
 *
 * The frames of a block follow each other from its offset; so does the audio
 * from its own. A record is appended only AFTER the .mov is fsync'ed, so it
 * describes bytes FAT already has. Reading stops at the first record that is
 * broken (CRC, magic) or that points past the end of the .mov.
 *
 * I/O. POSIX (open, read, write, lseek, fsync, ftruncate, unlink). On the P4
 * off_t is a signed 32 bits: writing never does lseek(SEEK_SET) to a data
 * offset - the position is kept in a uint64_t of our own, and the only seeks
 * are back to the header and SEEK_END. Reading (camera_drv_movie_info) uses seek_to(),
 * which on FAT works up to 4 GB (see the comment there).
 */
#include "camera_drv_movie.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "camera_drv_movie.c writes the PCM as it is: little-endian CPUs only (P4, arm64/x86 hosts)"
#endif

#define JOURNAL_HEAD    64U
#define REC_FIXED       32U
#define REC_MAX         (REC_FIXED + (8U * CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX) + 4U)
#define REC_MAGIC       0x31514C42U                    /* "BLQ1", little-endian */
#define JOURNAL_VERSION 1U
#define MOV_HEAD        36U                            /* ftyp(20) + wide(8) + mdat(8) */
#define WIDE_OFF        20U
#define HARD_LIMIT      (0xFFFFFFFFULL - (32ULL * 1024ULL * 1024ULL))  /* FAT: < 4 GB, room for the moov */
#define QT_EPOCH        2082844800U                    /* 1904 -> 1970 */
#define MAX_BOXES       64U
#define BOX_DEPTH       16U
#define WORK_DEFAULT    (64U * 1024U)
#define INFO_STACK      20U

/* ============================================================================================ */
/* Helpers                                                                                      */
/* ============================================================================================ */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (uint32_t i = 0U; i < 8U; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* CRC-32 (IEEE, zip's) with a 16-entry table: the journal moves ~120 B/s. */
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
        0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU, 0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
    };
    uint32_t c = 0xFFFFFFFFU;

    while (n > 0U) {
        c ^= *p;
        p++;
        n--;
        c = (c >> 4) ^ t[c & 15U];
        c = (c >> 4) ^ t[c & 15U];
    }
    return ~c;
}

static bool write_all(int fd, const void *p, size_t n)
{
    const uint8_t *q = (const uint8_t *)p;

    while (n > 0U) {
        ssize_t const r = write(fd, q, n);
        if ((r < 0) && (errno == EINTR)) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        q += r;
        n -= (size_t)r;
    }
    return true;
}

static bool read_all(int fd, void *p, size_t n)
{
    uint8_t *q = (uint8_t *)p;

    while (n > 0U) {
        ssize_t const r = read(fd, q, n);
        if ((r < 0) && (errno == EINTR)) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        q += r;
        n -= (size_t)r;
    }
    return true;
}

/* The real size of the file. On the P4 st_size is a signed 32-bit off_t: FAT
 * does not pass 4 GB, so reading it as a uint32_t is exact. */
static bool file_size(int fd, uint64_t *out)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return false;
    }
    if (sizeof(off_t) < 8U) {
        *out = (uint64_t)(uint32_t)st.st_size;
    } else {
        *out = (uint64_t)st.st_size;
    }
    return true;
}

/* Go to an absolute offset to READ. With a 64-bit off_t, directly. With a
 * 32-bit one (P4) and offsets >= 2 GB: IDF's FAT VFS hands the off_t to
 * f_lseek (FSIZE_t = DWORD) without looking at the sign, so the value
 * reinterpreted modulo 2^32 arrives right (vfs_fat.c, vfs_fat_lseek). */
static bool seek_to(int fd, uint64_t off)
{
    off_t o;

    if ((sizeof(off_t) >= 8U) || (off <= 0x7FFFFFFFULL)) {
        o = (off_t)off;
    } else {
        if (off > 0xFFFFFFFEULL) {
            return false;
        }
        o = (off_t)(int32_t)(uint32_t)off;
    }
    return lseek(fd, o, SEEK_SET) != (off_t)-1;
}

/* ============================================================================================ */
/* Names                                                                                        */
/* ============================================================================================ */

bool camera_drv_movie_journal_path(const char *mov_path, char *out, size_t cap)
{
    if ((mov_path == NULL) || (out == NULL)) {
        return false;
    }
    const char *slash = strrchr(mov_path, '/');
    size_t const dir = (slash != NULL) ? (size_t)(slash - mov_path) + 1U : 0U;
    const char *base = mov_path + dir;
    const char *dot = strrchr(base, '.');
    size_t const lb = (dot != NULL) ? (size_t)(dot - base) : strlen(base);

    if ((lb == 0U) || ((dir + 1U + lb + 4U + 1U) > cap)) {
        return false;
    }
    memcpy(out, mov_path, dir);
    out[dir] = '.';
    memcpy(out + dir + 1U, base, lb);
    memcpy(out + dir + 1U + lb, ".idx", 5U);
    return true;
}

bool camera_drv_movie_part_path(const char *mov_path, uint32_t n, char *out, size_t cap)
{
    if ((mov_path == NULL) || (out == NULL) || (n < 2U) || (n > 999U)) {
        return false;
    }
    const char *slash = strrchr(mov_path, '/');
    const char *base = (slash != NULL) ? (slash + 1) : mov_path;
    const char *dot = strrchr(base, '.');
    size_t const lp = (dot != NULL) ? (size_t)(dot - mov_path) : strlen(mov_path);
    const char *ext = (dot != NULL) ? dot : "";
    char suf[8];
    size_t k = 1U;

    suf[0] = '_';
    if (n >= 100U) {
        suf[k++] = (char)('0' + (n / 100U));
    }
    if (n >= 10U) {
        suf[k++] = (char)('0' + ((n / 10U) % 10U));
    }
    suf[k++] = (char)('0' + (n % 10U));
    suf[k] = '\0';
    size_t const le = strlen(ext);
    if ((lp + k + le + 1U) > cap) {
        return false;
    }
    memcpy(out, mov_path, lp);
    memcpy(out + lp, suf, k);
    memcpy(out + lp + k, ext, le + 1U);
    return true;
}

/* ============================================================================================ */
/* The journal's header                                                                         */
/* ============================================================================================ */

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t rotation;
    bool     audio;
    uint32_t unix_time;
    uint64_t mdat_head;
} jhead_t;

static void jhead_put(uint8_t *b, const jhead_t *c)
{
    memset(b, 0, JOURNAL_HEAD);
    memcpy(b, "KMOVIDX1", 8U);
    put_le32(b + 8, JOURNAL_VERSION);
    put_le16(b + 12, c->width);
    put_le16(b + 14, c->height);
    put_le16(b + 16, c->fps);
    put_le16(b + 18, c->audio ? 1U : 0U);
    put_le32(b + 20, c->unix_time);
    put_le64(b + 24, c->mdat_head);
    put_le16(b + 32, c->rotation);
    put_le32(b + 60, crc32_ieee(b, 60U));
}

/* 0 = good; -1 = garbage (magic or CRC); -2 = an unknown version (nothing is touched). */
static int jhead_get(const uint8_t *b, size_t n, jhead_t *c)
{
    if ((n < JOURNAL_HEAD) || (memcmp(b, "KMOVIDX1", 8U) != 0)) {
        return -1;
    }
    if (crc32_ieee(b, 60U) != get_le32(b + 60)) {
        return -1;
    }
    if (get_le32(b + 8) != JOURNAL_VERSION) {
        return -2;
    }
    c->width = get_le16(b + 12);
    c->height = get_le16(b + 14);
    c->fps = get_le16(b + 16);
    c->audio = (get_le16(b + 18) & 1U) != 0U;
    c->unix_time = get_le32(b + 20);
    c->mdat_head = get_le64(b + 24);
    c->rotation = get_le16(b + 32);
    if ((c->width == 0U) || (c->height == 0U) || (c->fps == 0U) || ((CAMERA_DRV_MOVIE_HZ % c->fps) != 0U)) {
        return -1;
    }
    if ((c->rotation != 0U) && (c->rotation != 90U) && (c->rotation != 180U) && (c->rotation != 270U)) {
        return -1;
    }
    return 0;
}

/* ============================================================================================ */
/* The journal's reader (all in memory if it fits; in pieces if not)                            */
/* ============================================================================================ */

typedef struct {
    int      fd;
    uint8_t *b;
    size_t   cap;
    size_t   n;
    size_t   i;
    bool     eof;
    bool     whole;
} jreader_t;

typedef struct {
    uint32_t nfr;
    uint32_t nau;
    uint32_t num;
    uint64_t off_v;
    uint64_t off_a;
    const uint8_t *fr;          /* nfr x {size, dup}, LE; valid until the next read */
} jrec_t;

static bool jreader_fill(jreader_t *l)
{
    if (((l->n - l->i) >= REC_MAX) || l->eof) {
        return true;
    }
    memmove(l->b, l->b + l->i, l->n - l->i);
    l->n -= l->i;
    l->i = 0U;
    while ((l->n < l->cap) && !l->eof) {
        ssize_t const r = read(l->fd, l->b + l->n, l->cap - l->n);
        if ((r < 0) && (errno == EINTR)) {
            continue;
        }
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            l->eof = true;
        }
        l->n += (size_t)r;
    }
    return true;
}

/* Read from the start; leave the cursor after the header. */
static bool jreader_rewind(jreader_t *l)
{
    if (l->whole) {
        l->i = JOURNAL_HEAD;
        return true;
    }
    if (lseek(l->fd, 0, SEEK_SET) == (off_t)-1) {
        return false;
    }
    l->n = 0U;
    l->i = 0U;
    l->eof = false;
    if (!jreader_fill(l)) {
        return false;
    }
    if (l->eof) {
        l->whole = true;                /* it all fit: the next passes do not read */
    }
    if (l->n < JOURNAL_HEAD) {
        return false;
    }
    l->i = JOURNAL_HEAD;
    return true;
}

/* 1 = a valid record (shape and CRC), 0 = the end or a broken one, -1 = an I/O error. */
static int jreader_next(jreader_t *l, jrec_t *r)
{
    if (!jreader_fill(l)) {
        return -1;
    }
    size_t const avail = l->n - l->i;
    if (avail < (REC_FIXED + 4U)) {
        return 0;
    }
    const uint8_t *p = l->b + l->i;
    if (get_le32(p) != REC_MAGIC) {
        return 0;
    }
    uint32_t const nfr = get_le32(p + 4);
    if (nfr > CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX) {
        return 0;
    }
    size_t const len = REC_FIXED + (8U * (size_t)nfr) + 4U;
    if (avail < len) {
        return 0;
    }
    if (crc32_ieee(p, len - 4U) != get_le32(p + len - 4U)) {
        return 0;
    }
    r->nfr = nfr;
    r->nau = get_le32(p + 8);
    r->num = get_le32(p + 12);
    r->off_v = get_le64(p + 16);
    r->off_a = get_le64(p + 24);
    r->fr = p + REC_FIXED;
    l->i += len;
    return 1;
}

/* ============================================================================================ */
/* The journal's summary and the walk over the video samples                                    */
/* ============================================================================================ */

typedef struct {
    uint32_t nrec;              /* valid records (the rest are ignored)                         */
    uint32_t frames;            /* frames with bytes                                            */
    uint64_t nv0;               /* slots of the records (frames + repeats)                      */
    uint32_t tail;              /* repeats of the last frame to cover the audio                 */
    uint64_t nv;                /* total slots = nv0 + tail                                     */
    uint32_t nvch;              /* video chunks                                                 */
    uint32_t stsc_v;            /* entries of the video stsc                                    */
    uint64_t na;                /* audio samples                                                */
    uint32_t nach;
    uint32_t stsc_a;
    uint64_t data_end;          /* the end of the last byte referenced                          */
    bool     co64;
} summary_t;

typedef void (*sample_cb_t)(void *ctx, uint64_t off, uint32_t size, bool new_chunk);

/* Walk the video samples in order: repeats (they point to the previous frame, or
 * to the file's first one if it is itself the first) and frames. new_chunk = a
 * chunk starts here (stco). */
static int walk_video(jreader_t *l, const summary_t *rs, sample_cb_t cb, void *ctx)
{
    if (!jreader_rewind(l)) {
        return -1;
    }
    bool have_prev = false;
    uint64_t poff = 0U;
    uint32_t psize = 0U;

    for (uint32_t k = 0U; k < rs->nrec; k++) {
        jrec_t r;
        int const e = jreader_next(l, &r);
        if (e <= 0) {
            return -1;              /* pass 1 already validated it: this should not happen */
        }
        uint64_t off = r.off_v;
        for (uint32_t j = 0U; j < r.nfr; j++) {
            uint32_t const size = get_le32(r.fr + (8U * j));
            uint32_t const dup = get_le32(r.fr + (8U * j) + 4U);
            for (uint32_t d = 0U; d < dup; d++) {
                if (have_prev) {
                    cb(ctx, poff, psize, true);
                } else {
                    cb(ctx, off, size, true);
                }
            }
            cb(ctx, off, size, (j == 0U) || (dup > 0U));
            poff = off;
            psize = size;
            have_prev = true;
            off += size;
        }
    }
    for (uint32_t t = 0U; (t < rs->tail) && have_prev; t++) {
        cb(ctx, poff, psize, true);
    }
    return 0;
}

/* stsc: an entry only where the number of samples per chunk changes. */
typedef struct out out_t;
typedef struct {
    out_t   *s;                 /* NULL = only count                                            */
    uint32_t chunk;
    uint32_t run_spc;
    uint32_t runs;
    uint32_t spc;               /* samples of the chunk in progress (video)                     */
    bool     open;
    uint64_t samples;
    uint64_t chunks;
} stsc_t;

static void o_u32(out_t *s, uint32_t v);

static void stsc_chunk(stsc_t *t, uint32_t spc)
{
    t->chunk++;
    t->chunks++;
    if ((t->runs == 0U) || (spc != t->run_spc)) {
        t->runs++;
        t->run_spc = spc;
        if (t->s != NULL) {
            o_u32(t->s, t->chunk);
            o_u32(t->s, spc);
            o_u32(t->s, 1U);
        }
    }
}

static void cb_stsc(void *ctx, uint64_t off, uint32_t size, bool new_chunk)
{
    stsc_t *t = (stsc_t *)ctx;

    (void)off;
    (void)size;
    if (new_chunk) {
        if (t->open) {
            stsc_chunk(t, t->spc);
        }
        t->open = true;
        t->spc = 0U;
    }
    t->spc++;
    t->samples++;
}

static void stsc_close(stsc_t *t)
{
    if (t->open) {
        stsc_chunk(t, t->spc);
    }
    t->open = false;
}

/* Pass 1: validate the records (shape + limits against the real size of the .mov) and count. */
static int summarize(jreader_t *l, const jhead_t *c, uint64_t eof, summary_t *rs)
{
    memset(rs, 0, sizeof *rs);
    if (!jreader_rewind(l)) {
        return -1;
    }
    uint64_t const data0 = c->mdat_head + 16U;
    uint32_t const spf = CAMERA_DRV_MOVIE_HZ / c->fps;
    uint32_t a_run = 0U;

    for (;;) {
        jrec_t r;
        int const e = jreader_next(l, &r);
        if (e < 0) {
            return -1;
        }
        if (e == 0) {
            break;
        }
        if ((r.nfr == 0U) && (r.nau == 0U)) {
            break;
        }
        uint64_t tv = 0U;
        uint64_t dup = 0U;
        for (uint32_t j = 0U; j < r.nfr; j++) {
            tv += get_le32(r.fr + (8U * j));
            dup += get_le32(r.fr + (8U * j) + 4U);
        }
        if ((r.nfr != 0U) && ((r.off_v < data0) || ((r.off_v + tv) > eof))) {
            break;
        }
        if ((r.nau != 0U) && (!c->audio || (r.off_a < data0) || ((r.off_a + (2ULL * r.nau)) > eof))) {
            break;
        }
        rs->nrec++;
        rs->frames += r.nfr;
        rs->nv0 += (uint64_t)r.nfr + dup;
        if ((r.nfr != 0U) && ((r.off_v + tv) > rs->data_end)) {
            rs->data_end = r.off_v + tv;
        }
        if (r.nau != 0U) {
            rs->na += r.nau;
            rs->nach++;
            if ((rs->stsc_a == 0U) || (r.nau != a_run)) {
                rs->stsc_a++;
                a_run = r.nau;
            }
            if ((r.off_a + (2ULL * r.nau)) > rs->data_end) {
                rs->data_end = r.off_a + (2ULL * r.nau);
            }
        }
    }
    if ((rs->nv0 != 0U) && (rs->na != 0U)) {
        uint64_t const audio_slots = (rs->na + (spf / 2U)) / spf;
        if (audio_slots > rs->nv0) {
            rs->tail = (uint32_t)(audio_slots - rs->nv0);
        }
    }
    if (rs->nv0 == 0U) {
        return 0;
    }
    /* Pass 2: the video chunks (they depend on the repeats). */
    stsc_t t;
    memset(&t, 0, sizeof t);
    if (walk_video(l, rs, cb_stsc, &t) != 0) {
        return -1;
    }
    stsc_close(&t);
    rs->nv = t.samples;
    rs->nvch = (uint32_t)t.chunks;
    rs->stsc_v = t.runs;
    rs->co64 = rs->data_end > 0xFFFFFFFFULL;
    return 0;
}

/* ============================================================================================ */
/* Box output: a dry pass (measures) and a real pass (writes)                                   */
/* ============================================================================================ */

struct out {
    int      fd;
    uint8_t *b;
    size_t   cap;
    size_t   n;
    uint64_t total;
    bool     dry;
    bool     err;
    uint32_t size[MAX_BOXES];
    uint32_t k;
    uint32_t np;
    uint32_t stack[BOX_DEPTH];
    uint64_t start[BOX_DEPTH];
};

static void o_flush(out_t *s)
{
    if ((s->n != 0U) && !s->err && !write_all(s->fd, s->b, s->n)) {
        s->err = true;
    }
    s->n = 0U;
}

/* p == NULL writes zeros. */
static void o_bytes(out_t *s, const void *p, size_t n)
{
    s->total += n;
    if (s->dry || s->err) {
        return;
    }
    const uint8_t *q = (const uint8_t *)p;
    while (n > 0U) {
        size_t k = s->cap - s->n;
        if (k > n) {
            k = n;
        }
        if (q != NULL) {
            memcpy(s->b + s->n, q, k);
        } else {
            memset(s->b + s->n, 0, k);
        }
        s->n += k;
        n -= k;
        if (q != NULL) {
            q += k;
        }
        if (s->n == s->cap) {
            o_flush(s);
        }
    }
}

static void o_zeros(out_t *s, size_t n)
{
    o_bytes(s, NULL, n);
}

static void o_u8(out_t *s, uint8_t v)
{
    o_bytes(s, &v, 1U);
}

static void o_u16(out_t *s, uint16_t v)
{
    uint8_t const b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    o_bytes(s, b, 2U);
}

static void o_u32(out_t *s, uint32_t v)
{
    uint8_t b[4];
    put_be32(b, v);
    o_bytes(s, b, 4U);
}

static void o_u64(out_t *s, uint64_t v)
{
    o_u32(s, (uint32_t)(v >> 32));
    o_u32(s, (uint32_t)v);
}

static void o_4cc(out_t *s, const char *t)
{
    o_bytes(s, t, 4U);
}

static void box_begin(out_t *s, const char *type)
{
    uint32_t const idx = s->k++;

    if ((idx >= MAX_BOXES) || (s->np >= BOX_DEPTH)) {
        s->err = true;
        return;
    }
    s->stack[s->np] = idx;
    s->start[s->np] = s->total;
    s->np++;
    o_u32(s, s->dry ? 0U : s->size[idx]);
    o_4cc(s, type);
}

static void box_end(out_t *s)
{
    if (s->np == 0U) {
        s->err = true;
        return;
    }
    s->np--;
    if (s->dry) {
        s->size[s->stack[s->np]] = (uint32_t)(s->total - s->start[s->np]);
    }
}

/* tkhd's presentation matrix (16.16, except u, v, w which are 2.30). */
static void write_matrix(out_t *s, uint16_t rotation, uint32_t width, uint32_t height)
{
    int32_t a = 0x10000;
    int32_t b = 0;
    int32_t c = 0;
    int32_t d = 0x10000;
    int32_t tx = 0;
    int32_t ty = 0;

    switch (rotation) {
    case 90U:
        a = 0;
        b = 0x10000;
        c = -0x10000;
        d = 0;
        tx = (int32_t)(height << 16);
        break;
    case 180U:
        a = -0x10000;
        d = -0x10000;
        tx = (int32_t)(width << 16);
        ty = (int32_t)(height << 16);
        break;
    case 270U:
        a = 0;
        b = -0x10000;
        c = 0x10000;
        d = 0;
        ty = (int32_t)(width << 16);
        break;
    default:
        break;
    }
    o_u32(s, (uint32_t)a);
    o_u32(s, (uint32_t)b);
    o_u32(s, 0U);
    o_u32(s, (uint32_t)c);
    o_u32(s, (uint32_t)d);
    o_u32(s, 0U);
    o_u32(s, (uint32_t)tx);
    o_u32(s, (uint32_t)ty);
    o_u32(s, 0x40000000U);
}

static void write_hdlr(out_t *s, const char *comp, const char *sub, const char *name)
{
    box_begin(s, "hdlr");
    o_u32(s, 0U);
    o_4cc(s, comp);
    o_4cc(s, sub);
    o_zeros(s, 12U);
    size_t const n = strlen(name);
    o_u8(s, (uint8_t)n);
    o_bytes(s, name, n);
    box_end(s);
}

static void write_dinf(out_t *s)
{
    box_begin(s, "dinf");
    box_begin(s, "dref");
    o_u32(s, 0U);
    o_u32(s, 1U);
    box_begin(s, "url ");
    o_u32(s, 1U);                             /* the data is in this very file */
    box_end(s);
    box_end(s);
    box_end(s);
}

static void write_tkhd(out_t *s, uint32_t id, uint32_t date, uint32_t ms, bool sound, uint16_t rotation,
                       uint32_t width, uint32_t height)
{
    box_begin(s, "tkhd");
    o_u32(s, 3U);                             /* version 0, flags: enabled + in the movie */
    o_u32(s, date);
    o_u32(s, date);
    o_u32(s, id);
    o_u32(s, 0U);
    o_u32(s, ms);
    o_zeros(s, 8U);
    o_u16(s, 0U);                             /* layer */
    o_u16(s, sound ? 1U : 0U);                /* alternate group */
    o_u16(s, sound ? 0x0100U : 0U);           /* volume */
    o_u16(s, 0U);
    write_matrix(s, sound ? 0U : rotation, width, height);
    o_u32(s, sound ? 0U : (width << 16));
    o_u32(s, sound ? 0U : (height << 16));
    box_end(s);
}

static void write_mdhd(out_t *s, uint32_t date, uint64_t dur)
{
    box_begin(s, "mdhd");
    o_u32(s, 0U);
    o_u32(s, date);
    o_u32(s, date);
    o_u32(s, CAMERA_DRV_MOVIE_HZ);
    o_u32(s, (dur > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)dur);
    o_u16(s, 0x7FFFU);                        /* language: undetermined (as ffmpeg) */
    o_u16(s, 0U);
    box_end(s);
}

typedef struct {
    out_t *s;
    bool   co64;
} table_t;

static void cb_stsz(void *ctx, uint64_t off, uint32_t size, bool new_chunk)
{
    (void)off;
    (void)new_chunk;
    o_u32(((table_t *)ctx)->s, size);
}

static void cb_stco(void *ctx, uint64_t off, uint32_t size, bool new_chunk)
{
    table_t *t = (table_t *)ctx;

    (void)size;
    if (!new_chunk) {
        return;
    }
    if (t->co64) {
        o_u64(t->s, off);
    } else {
        o_u32(t->s, (uint32_t)off);
    }
}

/* Audio: the chunks are the records that have samples. mode 0 = stsc, 1 = stco. */
static void audio_tables(out_t *s, jreader_t *l, const summary_t *rs, int mode)
{
    if (!jreader_rewind(l)) {
        s->err = true;
        return;
    }
    stsc_t t;
    memset(&t, 0, sizeof t);
    t.s = s;
    for (uint32_t k = 0U; k < rs->nrec; k++) {
        jrec_t r;
        if (jreader_next(l, &r) <= 0) {
            s->err = true;
            return;
        }
        if (r.nau == 0U) {
            continue;
        }
        if (mode == 0) {
            stsc_chunk(&t, r.nau);
        } else if (rs->co64) {
            o_u64(s, r.off_a);
        } else {
            o_u32(s, (uint32_t)r.off_a);
        }
    }
}

static void write_moov(out_t *s, jreader_t *l, const summary_t *rs, const jhead_t *c)
{
    uint32_t const spf = CAMERA_DRV_MOVIE_HZ / c->fps;
    uint32_t const date = (c->unix_time != 0U) ? (c->unix_time + QT_EPOCH) : 0U;
    uint64_t const dur_v = rs->nv * spf;
    uint32_t const ms_v = (uint32_t)(((dur_v * 1000U) + CAMERA_DRV_MOVIE_HZ - 1U) / CAMERA_DRV_MOVIE_HZ);
    uint32_t const ms_a = (uint32_t)(((rs->na * 1000U) + CAMERA_DRV_MOVIE_HZ - 1U) / CAMERA_DRV_MOVIE_HZ);
    bool const sound = c->audio && (rs->na > 0U);
    const char *co = rs->co64 ? "co64" : "stco";

    box_begin(s, "moov");

    box_begin(s, "mvhd");
    o_u32(s, 0U);
    o_u32(s, date);
    o_u32(s, date);
    o_u32(s, 1000U);
    o_u32(s, (ms_v > ms_a) ? ms_v : ms_a);
    o_u32(s, 0x00010000U);                    /* rate 1.0 */
    o_u16(s, 0x0100U);                        /* volume 1.0 */
    o_zeros(s, 10U);
    write_matrix(s, 0U, 0U, 0U);
    o_zeros(s, 24U);                          /* preview, poster, selection, current time */
    o_u32(s, sound ? 3U : 2U);                /* next track id */
    box_end(s);

    /* ---- video ---- */
    box_begin(s, "trak");
    write_tkhd(s, 1U, date, ms_v, false, c->rotation, c->width, c->height);
    box_begin(s, "mdia");
    write_mdhd(s, date, dur_v);
    write_hdlr(s, "mhlr", "vide", "VideoHandler");
    box_begin(s, "minf");
    box_begin(s, "vmhd");
    o_u32(s, 1U);
    o_zeros(s, 8U);
    box_end(s);
    write_hdlr(s, "dhlr", "url ", "DataHandler");
    write_dinf(s);
    box_begin(s, "stbl");

    box_begin(s, "stsd");
    o_u32(s, 0U);
    o_u32(s, 1U);
    box_begin(s, "jpeg");
    o_zeros(s, 6U);
    o_u16(s, 1U);                             /* data reference */
    o_u16(s, 0U);
    o_u16(s, 0U);
    o_u32(s, 0U);                             /* vendor */
    o_u32(s, 0x200U);                         /* temporal quality */
    o_u32(s, 0x200U);                         /* spatial quality */
    o_u16(s, c->width);
    o_u16(s, c->height);
    o_u32(s, 0x00480000U);                    /* 72 dpi */
    o_u32(s, 0x00480000U);
    o_u32(s, 0U);
    o_u16(s, 1U);                             /* frames per sample */
    {
        static const char comp[] = "Photo - JPEG";
        uint8_t name[32];
        memset(name, 0, sizeof name);
        name[0] = (uint8_t)(sizeof comp - 1U);
        memcpy(name + 1, comp, sizeof comp - 1U);
        o_bytes(s, name, sizeof name);
    }
    o_u16(s, 24U);                            /* depth */
    o_u16(s, 0xFFFFU);                        /* no colour table */
    box_begin(s, "fiel");
    o_u8(s, 1U);                              /* progressive */
    o_u8(s, 0U);
    box_end(s);
    box_begin(s, "pasp");
    o_u32(s, 1U);
    o_u32(s, 1U);
    box_end(s);
    box_end(s);                               /* jpeg */
    box_end(s);                               /* stsd */

    box_begin(s, "stts");                     /* constant cadence: ONE entry */
    o_u32(s, 0U);
    o_u32(s, 1U);
    o_u32(s, (uint32_t)rs->nv);
    o_u32(s, spf);
    box_end(s);

    box_begin(s, "stsc");
    o_u32(s, 0U);
    o_u32(s, rs->stsc_v);
    {
        stsc_t t;
        memset(&t, 0, sizeof t);
        t.s = s;
        if (walk_video(l, rs, cb_stsc, &t) != 0) {
            s->err = true;
        }
        stsc_close(&t);
    }
    box_end(s);

    box_begin(s, "stsz");
    o_u32(s, 0U);
    o_u32(s, 0U);
    o_u32(s, (uint32_t)rs->nv);
    {
        table_t t = { .s = s, .co64 = false };
        if (walk_video(l, rs, cb_stsz, &t) != 0) {
            s->err = true;
        }
    }
    box_end(s);

    box_begin(s, co);
    o_u32(s, 0U);
    o_u32(s, rs->nvch);
    {
        table_t t = { .s = s, .co64 = rs->co64 };
        if (walk_video(l, rs, cb_stco, &t) != 0) {
            s->err = true;
        }
    }
    box_end(s);

    box_end(s);                               /* stbl */
    box_end(s);                               /* minf */
    box_end(s);                               /* mdia */
    box_end(s);                               /* trak */

    /* ---- audio ---- */
    if (sound) {
        box_begin(s, "trak");
        write_tkhd(s, 2U, date, ms_a, true, 0U, 0U, 0U);
        box_begin(s, "mdia");
        write_mdhd(s, date, rs->na);
        write_hdlr(s, "mhlr", "soun", "SoundHandler");
        box_begin(s, "minf");
        box_begin(s, "smhd");
        o_u32(s, 0U);
        o_u32(s, 0U);
        box_end(s);
        write_hdlr(s, "dhlr", "url ", "DataHandler");
        write_dinf(s);
        box_begin(s, "stbl");

        box_begin(s, "stsd");
        o_u32(s, 0U);
        o_u32(s, 1U);
        box_begin(s, "sowt");                 /* 16-bit little-endian PCM, version 0 */
        o_zeros(s, 6U);
        o_u16(s, 1U);
        o_u16(s, 0U);
        o_u16(s, 0U);
        o_u32(s, 0U);
        o_u16(s, 1U);                         /* channels */
        o_u16(s, 16U);                        /* bits */
        o_u16(s, 0U);
        o_u16(s, 0U);
        o_u32(s, CAMERA_DRV_MOVIE_HZ << 16);
        box_begin(s, "chan");                 /* mono (kAudioChannelLayoutTag_Mono) */
        o_u32(s, 0U);
        o_u32(s, 0x00640001U);
        o_u32(s, 0U);
        o_u32(s, 0U);
        box_end(s);
        box_end(s);                           /* sowt */
        box_end(s);                           /* stsd */

        box_begin(s, "stts");
        o_u32(s, 0U);
        o_u32(s, 1U);
        o_u32(s, (rs->na > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)rs->na);
        o_u32(s, 1U);
        box_end(s);

        box_begin(s, "stsc");
        o_u32(s, 0U);
        o_u32(s, rs->stsc_a);
        audio_tables(s, l, rs, 0);
        box_end(s);

        box_begin(s, "stsz");
        o_u32(s, 0U);
        o_u32(s, 2U);                         /* bytes per sample (as ffmpeg) */
        o_u32(s, (rs->na > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)rs->na);
        box_end(s);

        box_begin(s, co);
        o_u32(s, 0U);
        o_u32(s, rs->nach);
        audio_tables(s, l, rs, 1);
        box_end(s);

        box_end(s);                           /* stbl */
        box_end(s);                           /* minf */
        box_end(s);                           /* mdia */
        box_end(s);                           /* trak */
    }
    box_end(s);                               /* moov */
}

/* ============================================================================================ */
/* Building the moov from the journal (a normal close, and the repair)                          */
/* ============================================================================================ */

/* fd_mov is open for read and write; fd_idx for read. eof = the .mov's known size
 * (a close) or 0 = look it up (the repair, which also trims what is left over). */
static camera_drv_movie_error_t build_moov(int fd_mov, int fd_idx, uint64_t eof, bool repair, uint8_t *buf, size_t bytes,
                                camera_drv_movie_repair_t *info)
{
    size_t const half = (bytes / 2U) & ~(size_t)15U;

    if (half < ((2U * REC_MAX) + JOURNAL_HEAD)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    jreader_t l;
    memset(&l, 0, sizeof l);
    l.fd = fd_idx;
    l.b = buf;
    l.cap = half;
    if (!jreader_rewind(&l)) {
        /* not even a whole header */
        return (l.n < JOURNAL_HEAD) ? CAMERA_DRV_MOVIE_EMPTY : CAMERA_DRV_MOVIE_ERROR_IO;
    }
    jhead_t c;
    memset(&c, 0, sizeof c);
    int const ec = jhead_get(l.b, l.n, &c);
    if (ec == -1) {
        return CAMERA_DRV_MOVIE_EMPTY;
    }
    if (ec == -2) {
        return CAMERA_DRV_MOVIE_ERROR_JOURNAL;
    }

    if ((eof == 0U) && !file_size(fd_mov, &eof)) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    summary_t rs;
    if (summarize(&l, &c, eof, &rs) != 0) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    if (rs.nv0 == 0U) {
        return CAMERA_DRV_MOVIE_EMPTY;
    }

    uint64_t fin = eof;
    if (repair && (rs.data_end < eof)) {
        /* Trim what did not arrive whole. ftruncate with a 32-bit off_t only below 2 GB; above,
         * those bytes stay inside mdat with nothing referencing them (harmless). */
        if (((sizeof(off_t) >= 8U) || (rs.data_end <= 0x7FFFFFFFULL)) && (ftruncate(fd_mov, (off_t)rs.data_end) == 0)) {
            fin = rs.data_end;
        }
    }
    /* (with a 32-bit off_t and > 2 GB the value comes back negative, but not -1: FAT is < 4 GB) */
    if (lseek(fd_mov, 0, SEEK_END) == (off_t)-1) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }

    out_t s;
    memset(&s, 0, sizeof s);
    s.fd = fd_mov;
    s.b = buf + half;
    s.cap = bytes - half;
    s.dry = true;
    write_moov(&s, &l, &rs, &c);
    if (s.err || (s.np != 0U)) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    s.dry = false;
    s.total = 0U;
    s.k = 0U;
    write_moov(&s, &l, &rs, &c);
    o_flush(&s);
    if (s.err || (fsync(fd_mov) != 0)) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }

    /* The mdat header: wide(8) + a 32-bit mdat, or a 64-bit mdat if it passes 4 GB. */
    uint8_t h[16];
    uint64_t const total = fin - c.mdat_head;
    if ((total - 8U) <= 0xFFFFFFFFULL) {
        put_be32(h, 8U);
        memcpy(h + 4, "wide", 4U);
        put_be32(h + 8, (uint32_t)(total - 8U));
        memcpy(h + 12, "mdat", 4U);
    } else {
        put_be32(h, 1U);
        memcpy(h + 4, "mdat", 4U);
        put_be32(h + 8, (uint32_t)(total >> 32));
        put_be32(h + 12, (uint32_t)total);
    }
    if ((lseek(fd_mov, (off_t)c.mdat_head, SEEK_SET) == (off_t)-1) || !write_all(fd_mov, h, 16U) ||
        (fsync(fd_mov) != 0)) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }

    if (info != NULL) {
        info->blocks = rs.nrec;
        info->slots = (uint32_t)rs.nv;
        info->audio_samples = rs.na;
        uint64_t const dv = rs.nv * (CAMERA_DRV_MOVIE_HZ / c.fps);
        uint64_t const d = (dv > rs.na) ? dv : rs.na;
        info->ms = (uint32_t)(((d * 1000U) + CAMERA_DRV_MOVIE_HZ - 1U) / CAMERA_DRV_MOVIE_HZ);
        info->ignored_bytes = (eof > rs.data_end) ? (eof - rs.data_end) : 0U;
    }
    return CAMERA_DRV_MOVIE_OK;
}

/* ============================================================================================ */
/* Writing                                                                                      */
/* ============================================================================================ */

/* Hand write() the ring's bytes [disk_pos, upto), in one or two pieces. */
static bool ring_flush(camera_drv_movie_t *m, uint64_t upto)
{
    while (m->disk_pos < upto) {
        size_t const idx = (size_t)(m->disk_pos % m->cfg.buf_bytes);
        size_t n = m->cfg.buf_bytes - idx;
        if (n > (upto - m->disk_pos)) {
            n = (size_t)(upto - m->disk_pos);
        }
        if (!write_all(m->fd, m->cfg.buf + idx, n)) {
            return false;
        }
        m->disk_pos += n;
    }
    return true;
}

/* Copy into the ring. When it is full, write up to the last block_bytes boundary of
 * the FILE: the big writes stay aligned to 512 KB (and to the cluster). */
static bool ring_put(camera_drv_movie_t *m, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    while (n > 0U) {
        size_t const room = m->cfg.buf_bytes - (size_t)(m->pos - m->disk_pos);
        if (room == 0U) {
            uint64_t upto = m->pos / m->cfg.block_bytes * m->cfg.block_bytes;
            if (upto <= m->disk_pos) {
                upto = m->pos;
            }
            if (!ring_flush(m, upto)) {
                return false;
            }
            continue;
        }
        size_t const idx = (size_t)(m->pos % m->cfg.buf_bytes);
        size_t k = m->cfg.buf_bytes - idx;
        if (k > room) {
            k = room;
        }
        if (k > n) {
            k = n;
        }
        memcpy(m->cfg.buf + idx, p, k);
        m->pos += k;
        p += k;
        n -= k;
    }
    return true;
}

static camera_drv_movie_error_t fail_broken(camera_drv_movie_t *m, camera_drv_movie_error_t e)
{
    m->broken = true;
    return e;
}

/* Close the block in progress: the audio chunk (if final, all of it; if not, only whole
 * slots, so that the pending audio always starts on a grid boundary), write, fsync and
 * append the record to the journal. */
static camera_drv_movie_error_t close_block(camera_drv_movie_t *m, bool final)
{
    size_t na = 0U;
    uint64_t off_a = 0U;

    if (m->cfg.audio && m->origin_ok && (m->a_n != 0U)) {
        na = final ? m->a_n : ((m->a_n / m->spf) * m->spf);
        if (na != 0U) {
            if ((m->pos + (2ULL * na)) > HARD_LIMIT) {
                return fail_broken(m, CAMERA_DRV_MOVIE_ERROR_FULL);
            }
            off_a = m->pos;
            if (!ring_put(m, m->cfg.audio_buf, na * 2U)) {
                return fail_broken(m, CAMERA_DRV_MOVIE_ERROR_IO);
            }
            memmove(m->cfg.audio_buf, m->cfg.audio_buf + na, (m->a_n - na) * 2U);
            m->a_n -= na;
            m->a_start += na;
            m->st.audio_samples += na;
        }
    }
    if ((m->nfr == 0U) && (na == 0U)) {
        return CAMERA_DRV_MOVIE_OK;
    }
    if (!ring_flush(m, m->pos) || (fsync(m->fd) != 0)) {
        return fail_broken(m, CAMERA_DRV_MOVIE_ERROR_IO);
    }

    uint8_t rec[REC_MAX];
    put_le32(rec, REC_MAGIC);
    put_le32(rec + 4, m->nfr);
    put_le32(rec + 8, (uint32_t)na);
    put_le32(rec + 12, m->st.blocks);
    put_le64(rec + 16, (m->nfr != 0U) ? m->video_off : 0U);
    put_le64(rec + 24, off_a);
    for (uint32_t j = 0U; j < m->nfr; j++) {
        put_le32(rec + REC_FIXED + (8U * j), m->fr[j].size);
        put_le32(rec + REC_FIXED + (8U * j) + 4U, m->fr[j].dup_before);
    }
    size_t const len = REC_FIXED + (8U * (size_t)m->nfr);
    put_le32(rec + len, crc32_ieee(rec, len));
    if (!write_all(m->jfd, rec, len + 4U) || (fsync(m->jfd) != 0)) {
        return fail_broken(m, CAMERA_DRV_MOVIE_ERROR_IO);
    }
    m->nfr = 0U;
    m->st.blocks++;
    m->st.bytes = m->pos;
    return CAMERA_DRV_MOVIE_OK;
}

/* Create the .mov and its journal and write the headers. The clock and audio state is left alone. */
static camera_drv_movie_error_t open_files(camera_drv_movie_t *m, const char *path)
{
    if ((path == NULL) || (strlen(path) >= CAMERA_DRV_MOVIE_PATH_MAX)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    strcpy(m->path, path);
    if (!camera_drv_movie_journal_path(path, m->journal_path, sizeof m->journal_path)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    m->fd = open(m->path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (m->fd < 0) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    m->jfd = open(m->journal_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (m->jfd < 0) {
        close(m->fd);
        m->fd = -1;                           /* a stale descriptor would be closed twice by camera_drv_movie_abort() */
        unlink(m->path);
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    m->pos = 0U;
    m->disk_pos = 0U;
    m->mdat_head = WIDE_OFF;
    m->nfr = 0U;
    m->next_slot = 0U;
    m->st.frames = 0U;
    m->st.slots = 0U;
    m->st.blocks = 0U;
    m->st.audio_samples = 0U;

    jhead_t c = { .width = m->cfg.width, .height = m->cfg.height, .fps = m->cfg.fps, .rotation = m->cfg.rotation,
                  .audio = m->cfg.audio, .unix_time = m->cfg.unix_time, .mdat_head = m->mdat_head };
    uint8_t head[JOURNAL_HEAD];
    jhead_put(head, &c);
    static const uint8_t init[MOV_HEAD] = {
        0, 0, 0, 20, 'f', 't', 'y', 'p', 'q', 't', ' ', ' ', 0, 0, 2, 0, 'q', 't', ' ', ' ',
        0, 0, 0, 8,  'w', 'i', 'd', 'e',
        0, 0, 0, 0,  'm', 'd', 'a', 't',   /* size 0 = "to the end" until it is closed */
    };
    if (!write_all(m->jfd, head, sizeof head) || (fsync(m->jfd) != 0) || !ring_put(m, init, sizeof init)) {
        close(m->fd);
        close(m->jfd);
        m->fd = -1;
        m->jfd = -1;
        unlink(m->path);
        unlink(m->journal_path);
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    m->st.bytes = m->pos;
    m->opened = true;
    m->broken = false;
    return CAMERA_DRV_MOVIE_OK;
}

camera_drv_movie_error_t camera_drv_movie_open(camera_drv_movie_t *m, const char *path, const camera_drv_movie_configuration_t *cfg)
{
    if ((m == NULL) || (cfg == NULL)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    memset(m, 0, sizeof *m);
    m->fd = -1;
    m->jfd = -1;
    m->cfg = *cfg;
    camera_drv_movie_configuration_t *c = &m->cfg;
    if (c->block_bytes == 0U) {
        c->block_bytes = CAMERA_DRV_MOVIE_BLOCK_DEFAULT;
    }
    if (c->limit_bytes == 0U) {
        c->limit_bytes = CAMERA_DRV_MOVIE_LIMIT_DEFAULT;
    }
    if ((c->width == 0U) || (c->height == 0U) || (c->fps == 0U) || ((CAMERA_DRV_MOVIE_HZ % c->fps) != 0U)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    if ((c->rotation != 0U) && (c->rotation != 90U) && (c->rotation != 180U) && (c->rotation != 270U)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    if (((c->block_bytes % 512U) != 0U) || (c->buf == NULL) || (c->buf_bytes < c->block_bytes) ||
        ((c->buf_bytes % c->block_bytes) != 0U) || (c->buf_bytes < 4096U)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    m->spf = CAMERA_DRV_MOVIE_HZ / c->fps;
    if (c->audio && ((c->audio_buf == NULL) || (c->audio_samples < (2U * (size_t)m->spf)))) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    return open_files(m, path);
}

/* When the origin is set (the first frame): the pending audio starts exactly at the origin. */
static void align_origin(camera_drv_movie_t *m)
{
    if (!m->a_ok) {
        m->a_ok = true;
        m->a_start = m->origin;
        m->a_n = 0U;
        return;
    }
    size_t const cap = m->cfg.audio_samples;
    int16_t *b = m->cfg.audio_buf;

    if ((m->a_start + m->a_n) <= m->origin) {
        m->st.audio_dropped += m->a_n;
        m->a_n = 0U;
    } else if (m->a_start <= m->origin) {
        size_t const t = (size_t)(m->origin - m->a_start);
        memmove(b, b + t, (m->a_n - t) * 2U);
        m->a_n -= t;
        m->st.audio_dropped += t;
    } else {
        /* the audio began after the first frame: silence in front */
        uint64_t const pre = m->a_start - m->origin;
        if (pre >= cap) {
            m->st.audio_dropped += m->a_n;
            m->a_n = 0U;
        } else {
            size_t const keep = (m->a_n < (cap - (size_t)pre)) ? m->a_n : (cap - (size_t)pre);
            memmove(b + (size_t)pre, b, keep * 2U);
            memset(b, 0, (size_t)pre * 2U);
            m->st.audio_dropped += m->a_n - keep;
            m->st.silence += pre;
            m->a_n = (size_t)pre + keep;
        }
    }
    m->a_start = m->origin;
}

camera_drv_movie_error_t camera_drv_movie_frame(camera_drv_movie_t *m, const void *jpeg, size_t len, uint64_t tick)
{
    if ((m == NULL) || !m->opened || m->broken) {
        return CAMERA_DRV_MOVIE_ERROR_STATE;
    }
    if ((jpeg == NULL) || (len == 0U) || (len > 0x7FFFFFFFU)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    if ((m->pos + len) > HARD_LIMIT) {
        return CAMERA_DRV_MOVIE_ERROR_FULL;
    }

    if (!m->origin_ok) {
        if (tick == CAMERA_DRV_MOVIE_NO_STAMP) {
            tick = (m->cfg.audio && m->a_ok) ? (m->a_start + m->a_n) : 0U;
        }
        m->origin = tick;
        m->origin_ok = true;
        m->slot0 = 0U;
        m->next_slot = 0U;
        if (m->cfg.audio) {
            align_origin(m);
        }
    }
    uint64_t const free_slot = m->slot0 + m->next_slot;      /* the next free global slot */
    uint64_t g;
    if (tick == CAMERA_DRV_MOVIE_NO_STAMP) {
        g = free_slot;
    } else {
        if (tick < m->origin) {
            m->st.dropped++;
            return CAMERA_DRV_MOVIE_DROPPED;
        }
        g = (tick - m->origin + (m->spf / 2U)) / m->spf;
    }
    if (g < free_slot) {
        m->st.dropped++;
        return CAMERA_DRV_MOVIE_DROPPED;
    }
    uint64_t const dup = g - free_slot;
    if (dup > (3600ULL * m->cfg.fps)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;                              /* an absurd stamp (> 1 h of gap) */
    }

    if (m->nfr == CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX) {
        camera_drv_movie_error_t const e = close_block(m, false);
        if (e != CAMERA_DRV_MOVIE_OK) {
            return e;
        }
    }
    if (m->nfr == 0U) {
        m->video_off = m->pos;
    }
    if (!ring_put(m, jpeg, len)) {
        return fail_broken(m, CAMERA_DRV_MOVIE_ERROR_IO);
    }
    m->fr[m->nfr].size = (uint32_t)len;
    m->fr[m->nfr].dup_before = (uint32_t)dup;
    m->nfr++;
    m->next_slot = (uint32_t)(g - m->slot0 + 1U);
    m->st.frames++;
    m->st.slots += (uint32_t)(1U + dup);
    m->st.duplicates += (uint32_t)dup;
    m->st.bytes = m->pos;

    /* A block by frames: with no audio, every second; with audio, only if the audio has stopped. */
    uint32_t limit = m->cfg.audio ? (2U * m->cfg.fps) : m->cfg.fps;
    if (limit > CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX) {
        limit = CAMERA_DRV_MOVIE_BLOCK_FRAMES_MAX;
    }
    if (m->nfr >= limit) {
        return close_block(m, false);
    }
    return CAMERA_DRV_MOVIE_OK;
}

/* Push n samples (pcm NULL = silence) to the pending audio; close a block about each second. */
static camera_drv_movie_error_t audio_push(camera_drv_movie_t *m, const int16_t *pcm, uint64_t n)
{
    size_t const cap = m->cfg.audio_samples;
    size_t const threshold = (CAMERA_DRV_MOVIE_HZ < (cap - m->spf)) ? (size_t)CAMERA_DRV_MOVIE_HZ : (cap - m->spf);
    int16_t *b = m->cfg.audio_buf;

    while (n > 0U) {
        size_t const room = cap - m->a_n;
        if (room == 0U) {
            if (m->origin_ok) {
                camera_drv_movie_error_t const e = close_block(m, false);
                if (e != CAMERA_DRV_MOVIE_OK) {
                    return e;
                }
            } else {
                /* before the first frame: a sliding window, the older half is thrown away */
                size_t const t = cap / 2U;
                memmove(b, b + t, (m->a_n - t) * 2U);
                m->a_n -= t;
                m->a_start += t;
                m->st.audio_dropped += t;
            }
            continue;
        }
        size_t const k = (n < room) ? (size_t)n : room;
        if (pcm != NULL) {
            memcpy(b + m->a_n, pcm, k * 2U);
            pcm += k;
        } else {
            memset(b + m->a_n, 0, k * 2U);
        }
        m->a_n += k;
        n -= k;
        if (m->origin_ok && (m->a_n >= threshold)) {
            camera_drv_movie_error_t const e = close_block(m, false);
            if (e != CAMERA_DRV_MOVIE_OK) {
                return e;
            }
        }
    }
    return CAMERA_DRV_MOVIE_OK;
}

camera_drv_movie_error_t camera_drv_movie_audio(camera_drv_movie_t *m, const int16_t *pcm, size_t n, uint64_t first_tick)
{
    if ((m == NULL) || !m->opened || m->broken) {
        return CAMERA_DRV_MOVIE_ERROR_STATE;
    }
    if (!m->cfg.audio || ((pcm == NULL) && (n != 0U))) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    if (n == 0U) {
        return CAMERA_DRV_MOVIE_OK;
    }
    if (!m->a_ok) {
        m->a_ok = true;
        m->a_start = (first_tick == CAMERA_DRV_MOVIE_NO_STAMP) ? 0U : first_tick;
        m->a_n = 0U;
    }
    uint64_t const expected = m->a_start + m->a_n;
    if (first_tick == CAMERA_DRV_MOVIE_NO_STAMP) {
        first_tick = expected;
    }
    if (first_tick > expected) {
        uint64_t const gap = first_tick - expected;
        if (gap > (3600ULL * CAMERA_DRV_MOVIE_HZ)) {
            return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
        }
        m->st.silence += gap;
        camera_drv_movie_error_t const e = audio_push(m, NULL, gap);
        if (e != CAMERA_DRV_MOVIE_OK) {
            return e;
        }
    } else if (first_tick < expected) {
        uint64_t const overlap = expected - first_tick;
        if (overlap >= n) {
            m->st.audio_dropped += n;
            return CAMERA_DRV_MOVIE_OK;
        }
        pcm += overlap;
        n -= (size_t)overlap;
        m->st.audio_dropped += overlap;
    }
    return audio_push(m, pcm, n);
}

bool camera_drv_movie_full(const camera_drv_movie_t *m)
{
    if ((m == NULL) || !m->opened) {
        return false;
    }
    uint64_t const expected = m->pos + (2ULL * m->a_n) + m->cfg.block_bytes + (24ULL * m->st.slots) + (1024ULL * 1024ULL);
    return expected >= m->cfg.limit_bytes;
}

/* Close the current file with its moov. final = empty ALL the audio (close) or only up to
 * the grid (split: the rest goes to the next file). */
static camera_drv_movie_error_t finish_file(camera_drv_movie_t *m, bool final)
{
    camera_drv_movie_error_t e = close_block(m, final);

    if ((e == CAMERA_DRV_MOVIE_OK) && !ring_flush(m, m->pos)) {
        e = fail_broken(m, CAMERA_DRV_MOVIE_ERROR_IO);
    }
    if (e == CAMERA_DRV_MOVIE_OK) {
        e = build_moov(m->fd, m->jfd, m->pos, false, m->cfg.buf, m->cfg.buf_bytes, NULL);
        if ((int)e < 0) {
            m->broken = true;
        }
    }
    close(m->fd);
    close(m->jfd);
    m->fd = -1;
    m->jfd = -1;
    m->opened = false;
    if (e == CAMERA_DRV_MOVIE_OK) {
        unlink(m->journal_path);
    } else if (e == CAMERA_DRV_MOVIE_EMPTY) {
        unlink(m->path);
        unlink(m->journal_path);
    } else {
        /* with an error the journal stays for camera_drv_movie_repair() */
    }
    return e;
}

camera_drv_movie_error_t camera_drv_movie_close(camera_drv_movie_t *m)
{
    if ((m == NULL) || !m->opened) {
        return CAMERA_DRV_MOVIE_ERROR_STATE;
    }
    if (m->broken) {
        camera_drv_movie_abort(m);
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    return finish_file(m, true);
}

camera_drv_movie_error_t camera_drv_movie_split(camera_drv_movie_t *m, const char *new_path)
{
    if ((m == NULL) || !m->opened || m->broken) {
        return CAMERA_DRV_MOVIE_ERROR_STATE;
    }
    if ((new_path == NULL) || (strlen(new_path) >= CAMERA_DRV_MOVIE_PATH_MAX)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    camera_drv_movie_error_t const e = finish_file(m, false);
    if ((int)e < 0) {
        return e;
    }
    /* The new file starts on the grid boundary where the audio was cut. */
    if (m->origin_ok) {
        if (m->cfg.audio) {
            m->slot0 = (m->a_start - m->origin) / m->spf;
        } else {
            m->slot0 += m->next_slot;
        }
    }
    m->st.parts++;
    return open_files(m, new_path);
}

void camera_drv_movie_abort(camera_drv_movie_t *m)
{
    if (m == NULL) {
        return;
    }
    if (m->fd >= 0) {
        close(m->fd);
    }
    if (m->jfd >= 0) {
        close(m->jfd);
    }
    m->fd = -1;
    m->jfd = -1;
    m->opened = false;
}

void camera_drv_movie_statistics(const camera_drv_movie_t *m, camera_drv_movie_statistics_t *out)
{
    if ((m != NULL) && (out != NULL)) {
        *out = m->st;
    }
}

/* ============================================================================================ */
/* Repair                                                                                       */
/* ============================================================================================ */

camera_drv_movie_error_t camera_drv_movie_repair(const char *mov_path, void *buf, size_t bytes, camera_drv_movie_repair_t *info)
{
    char jpath[CAMERA_DRV_MOVIE_PATH_MAX + 8U];

    if (info != NULL) {
        memset(info, 0, sizeof *info);
    }
    if (!camera_drv_movie_journal_path(mov_path, jpath, sizeof jpath)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    int const fdi = open(jpath, O_RDONLY);
    if (fdi < 0) {
        return (errno == ENOENT) ? CAMERA_DRV_MOVIE_NO_JOURNAL : CAMERA_DRV_MOVIE_ERROR_IO;
    }
    int const fdm = open(mov_path, O_RDWR);
    if (fdm < 0) {
        close(fdi);
        if (errno != ENOENT) {
            return CAMERA_DRV_MOVIE_ERROR_IO;
        }
        unlink(jpath);                        /* a journal without video: nothing to save */
        return CAMERA_DRV_MOVIE_EMPTY;
    }
    void *own = NULL;
    if ((buf == NULL) || (bytes < WORK_DEFAULT)) {
        own = malloc(WORK_DEFAULT);
        if (own == NULL) {
            close(fdm);
            close(fdi);
            return CAMERA_DRV_MOVIE_ERROR_IO;
        }
        buf = own;
        bytes = WORK_DEFAULT;
    }
    camera_drv_movie_error_t const e = build_moov(fdm, fdi, 0U, true, (uint8_t *)buf, bytes, info);
    free(own);
    close(fdm);
    close(fdi);
    if (e == CAMERA_DRV_MOVIE_OK) {
        unlink(jpath);
    } else if (e == CAMERA_DRV_MOVIE_EMPTY) {
        unlink(mov_path);
        unlink(jpath);
    } else {
        /* an error leaves both files as they are */
    }
    return e;
}

/**
 * @brief The names of up to @p max journals in @p folder (".NAME.idx"), noted
 *        first because deleting while reading a directory is not safe on FAT.
 * @return How many; -1 when the folder would not open.
 */
static int journal_names(const char *folder, char (*names)[CAMERA_DRV_MOVIE_REPAIR_NAME_LEN], int max)
{
    int n = 0;
    DIR *d = opendir(folder);

    if (d == NULL) {
        return -1;
    }
    struct dirent *de;
    while (((de = readdir(d)) != NULL) && (n < max)) {
        const char *s = de->d_name;
        size_t const l = strlen(s);
        if ((l < 6U) || (l >= CAMERA_DRV_MOVIE_REPAIR_NAME_LEN) || (s[0] != '.')
            || (strcasecmp(s + l - 4U, ".idx") != 0)) {
            continue;
        }
        memcpy(names[n], s, l + 1U);
        n++;
    }
    closedir(d);
    return n;
}

int camera_drv_movie_repair_directory(const char *folder, void *buf, size_t bytes)
{
    enum { MAX_NAMES = 16 };
    /* On the heap, not in a static: two repairs at once (two folders, two tasks)
     * would share it. A repair that finds no memory is an error, not a crash. */
    char (*names)[CAMERA_DRV_MOVIE_REPAIR_NAME_LEN] = malloc((size_t)MAX_NAMES * CAMERA_DRV_MOVIE_REPAIR_NAME_LEN);
    int ok = 0;

    if (names == NULL) {
        return -1;
    }
    /* A folder with more journals than the names held is read again: a movie mended
     * has no journal left, so the next pass finds the next ones. It stops when a
     * pass clears none (a journal that cannot be mended stays, and would loop). */
    for (int pass = 0; pass < 64; pass++) {
        int const n = journal_names(folder, names, MAX_NAMES);
        if (n < 0) {
            free(names);
            return (ok > 0) ? ok : -1;
        }
        int mended = 0;
        int gone   = 0;   /* journals that are no more: mended, or an empty movie removed */
        for (int i = 0; i < n; i++) {
            char path[CAMERA_DRV_MOVIE_PATH_MAX];
            size_t const lc = strlen(folder);
            size_t const ln = strlen(names[i]);
            /* ".VID_x.idx" -> "VID_x.mov" */
            if ((lc + 1U + (ln - 1U) + 1U) > sizeof path) {
                continue;
            }
            memcpy(path, folder, lc);
            path[lc] = '/';
            memcpy(path + lc + 1U, names[i] + 1, ln - 1U);
            memcpy(path + lc + 1U + ln - 1U - 4U, ".mov", 5U);
            camera_drv_movie_error_t const e = camera_drv_movie_repair(path, buf, bytes, NULL);
            if (e == CAMERA_DRV_MOVIE_OK) {
                mended++;
                gone++;
            } else if (e == CAMERA_DRV_MOVIE_EMPTY) {
                gone++;
            } else {
                /* stays, for another try */
            }
        }
        ok += mended;
        if ((n < MAX_NAMES) || (gone == 0)) {
            break;
        }
    }
    free(names);
    return ok;
}

/* ============================================================================================ */
/* Reading a header (the gallery)                                                               */
/* ============================================================================================ */

typedef struct {
    int      fd;
    camera_drv_movie_info_t *o;
    char     type[4];           /* the hdlr of the track in progress                            */
    uint16_t rotation;
    uint16_t width;
    uint16_t height;
    uint16_t tk_width;
    uint16_t tk_height;
    uint32_t scale;
    uint32_t delta;
    uint32_t nstts;
    uint32_t nstsz;
    bool     jpeg;
    bool     video_ok;
} info_ctx_t;

typedef struct {
    uint64_t ini;
    uint64_t fin;
    uint32_t depth;
    bool     trak_end;          /* a marker, not a range: the track's children are done          */
} walk_frame_t;

static bool read_at(int fd, uint64_t off, void *p, size_t n)
{
    return seek_to(fd, off) && read_all(fd, p, n);
}

static uint16_t rotation_of_matrix(const uint8_t *m)
{
    int32_t const a = (int32_t)get_be32(m);
    int32_t const b = (int32_t)get_be32(m + 4);
    int32_t const c = (int32_t)get_be32(m + 12);

    if (a > 0) {
        return 0U;
    }
    if (a < 0) {
        return 180U;
    }
    if ((b > 0) && (c < 0)) {
        return 90U;
    }
    if ((b < 0) && (c > 0)) {
        return 270U;
    }
    return 0U;
}

/* A track's children are all read: keep what it said. */
static void info_trak_done(info_ctx_t *x)
{
    if ((memcmp(x->type, "vide", 4U) == 0) && !x->video_ok) {
        x->video_ok = true;
        x->o->rotation = x->rotation;
        x->o->width = (x->width != 0U) ? x->width : x->tk_width;
        x->o->height = (x->height != 0U) ? x->height : x->tk_height;
        x->o->slots = x->nstsz;
        x->o->fps = ((x->nstts == 1U) && (x->delta != 0U) && ((x->scale % x->delta) == 0U))
                        ? (uint16_t)(x->scale / x->delta) : 0U;
    } else if (memcmp(x->type, "soun", 4U) == 0) {
        x->o->audio = true;
        x->o->audio_hz = x->scale;
        x->o->audio_samples = x->nstsz;
    }
}

/* One leaf box's content, @p b holding its first @p nb bytes (zero-filled past them). */
static void info_leaf(info_ctx_t *x, const uint8_t *t, const uint8_t *b, size_t nb)
{
    if ((memcmp(t, "mvhd", 4U) == 0) && (nb >= 20U)) {
        uint32_t const scale = (b[0] == 1U) ? get_be32(b + 20) : get_be32(b + 12);
        uint64_t const dur = (b[0] == 1U) ? (((uint64_t)get_be32(b + 24) << 32) | get_be32(b + 28))
                                          : get_be32(b + 16);
        if (scale != 0U) {
            x->o->ms = (uint32_t)(dur * 1000U / scale);
        }
    } else if ((memcmp(t, "tkhd", 4U) == 0) && (nb >= 84U) && (b[0] == 0U)) {
        x->rotation = rotation_of_matrix(b + 40);
        x->tk_width = (uint16_t)(get_be32(b + 76) >> 16);
        x->tk_height = (uint16_t)(get_be32(b + 80) >> 16);
    } else if ((memcmp(t, "mdhd", 4U) == 0) && (nb >= 16U)) {
        x->scale = (b[0] == 1U) ? get_be32(b + 20) : get_be32(b + 12);
    } else if ((memcmp(t, "hdlr", 4U) == 0) && (nb >= 12U) && (x->type[0] == '\0')) {
        memcpy(x->type, b + 8, 4U);           /* the first one (mdia); minf's is 'dhlr' */
    } else if ((memcmp(t, "stsd", 4U) == 0) && (nb >= (8U + 8U + 28U + 4U))) {
        /* first entry: size(4) type(4) + 6 reserved + 2 dref + 16 + width(2) height(2) */
        if (memcmp(b + 12, "jpeg", 4U) == 0) {
            x->jpeg = true;
            x->width = (uint16_t)(((uint32_t)b[16 + 24] << 8) | (uint32_t)b[16 + 25]);
            x->height = (uint16_t)(((uint32_t)b[16 + 26] << 8) | (uint32_t)b[16 + 27]);
        }
    } else if ((memcmp(t, "stts", 4U) == 0) && (nb >= 16U)) {
        x->nstts = get_be32(b + 4);
        x->delta = get_be32(b + 12);
    } else if ((memcmp(t, "stsz", 4U) == 0) && (nb >= 12U)) {
        x->nstsz = get_be32(b + 8);
    } else {
        /* any other box says nothing we need */
    }
}

/* Walks the boxes of [0, eof) with an explicit stack instead of recursion: the
 * containers (moov, mdia, minf, stbl, trak) push their children's range. */
static void info_walk(info_ctx_t *x, uint64_t eof)
{
    walk_frame_t st[INFO_STACK];
    size_t sp = 0U;

    st[sp].ini = 0U;
    st[sp].fin = eof;
    st[sp].depth = 0U;
    st[sp].trak_end = false;
    sp++;
    while (sp > 0U) {
        walk_frame_t *f = &st[sp - 1U];
        if (f->trak_end) {
            info_trak_done(x);
            sp--;
            continue;
        }
        if (((f->ini + 8U) > f->fin) || (f->depth >= 8U)) {
            sp--;
            continue;
        }
        uint8_t h[16];
        if (!read_at(x->fd, f->ini, h, 8U)) {
            sp--;
            continue;
        }
        uint64_t size = get_be32(h);
        uint64_t head_len = 8U;
        if (size == 1U) {
            if (!read_at(x->fd, f->ini + 8U, h + 8, 8U)) {
                sp--;
                continue;
            }
            size = ((uint64_t)get_be32(h + 8) << 32) | get_be32(h + 12);
            head_len = 16U;
        } else if (size == 0U) {
            size = f->fin - f->ini;
        } else {
            /* a plain 32-bit size */
        }
        if ((size < head_len) || ((f->ini + size) > f->fin)) {
            sp--;
            continue;
        }
        const uint8_t *t = h + 4;
        uint64_t const cu = f->ini + head_len;
        uint64_t const cf = f->ini + size;
        uint32_t const depth = f->depth;
        uint8_t b[96] = { 0 };
        size_t const nb = (size_t)(((cf - cu) < sizeof b) ? (cf - cu) : sizeof b);

        f->ini += size;                       /* the next sibling, once the children are done */
        bool const is_trak = memcmp(t, "trak", 4U) == 0;
        bool const is_box = (memcmp(t, "moov", 4U) == 0) || (memcmp(t, "mdia", 4U) == 0) ||
                            (memcmp(t, "minf", 4U) == 0) || (memcmp(t, "stbl", 4U) == 0);
        if (is_box || is_trak) {
            size_t const need = is_trak ? 2U : 1U;
            if ((sp + need) > INFO_STACK) {
                continue;
            }
            if (is_trak) {
                memset(x->type, 0, 4U);
                x->jpeg = false;
                x->rotation = 0U;
                x->scale = 0U;
                x->delta = 0U;
                x->nstts = 0U;
                x->nstsz = 0U;
                st[sp].ini = 0U;
                st[sp].fin = 0U;
                st[sp].depth = depth;
                st[sp].trak_end = true;       /* popped after its children */
                sp++;
            }
            st[sp].ini = cu;
            st[sp].fin = cf;
            st[sp].depth = depth + 1U;
            st[sp].trak_end = false;
            sp++;
        } else if ((nb != 0U) && read_at(x->fd, cu, b, nb)) {
            info_leaf(x, t, b, nb);
        } else {
            /* unreadable or empty: skip it */
        }
    }
}

camera_drv_movie_error_t camera_drv_movie_info(const char *path, camera_drv_movie_info_t *out)
{
    if ((path == NULL) || (out == NULL)) {
        return CAMERA_DRV_MOVIE_ERROR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    int const fd = open(path, O_RDONLY);
    if (fd < 0) {
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    uint64_t eof;
    if (!file_size(fd, &eof)) {
        close(fd);
        return CAMERA_DRV_MOVIE_ERROR_IO;
    }
    info_ctx_t x;
    memset(&x, 0, sizeof x);
    x.fd = fd;
    x.o = out;
    info_walk(&x, eof);
    close(fd);
    return x.video_ok ? CAMERA_DRV_MOVIE_OK : CAMERA_DRV_MOVIE_ERROR_JOURNAL;
}

const char *camera_drv_movie_error_string(camera_drv_movie_error_t e)
{
    switch (e) {
    case CAMERA_DRV_MOVIE_OK:
        return "ok";
    case CAMERA_DRV_MOVIE_DROPPED:
        return "frame dropped (slot taken)";
    case CAMERA_DRV_MOVIE_EMPTY:
        return "empty (deleted)";
    case CAMERA_DRV_MOVIE_NO_JOURNAL:
        return "no journal";
    case CAMERA_DRV_MOVIE_ERROR_ARGUMENT:
        return "bad parameter";
    case CAMERA_DRV_MOVIE_ERROR_IO:
        return "I/O error";
    case CAMERA_DRV_MOVIE_ERROR_FULL:
        return "file full (FAT limit)";
    case CAMERA_DRV_MOVIE_ERROR_JOURNAL:
        return "journal or header unreadable";
    case CAMERA_DRV_MOVIE_ERROR_STATE:
        return "invalid state";
    }
    return "?";
}
