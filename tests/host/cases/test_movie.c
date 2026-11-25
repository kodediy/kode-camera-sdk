/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_movie.c
 * @brief camera_drv_movie: a recording made, cut, repaired and split, read back.
 *
 * These cases judge the .mov files here, with a QuickTime
 * reader of their own that does not trust the muxer's own tables: it walks
 * ftyp, wide, mdat and moov, finds the video and the audio track, resolves
 * stsz, stsc and stco/co64 into byte offsets, and checks every sample's
 * bytes against what was fed in. No JPEG encoder is needed: the muxer never
 * parses a frame, so a frame here is SOI, an id, noise from that id and EOI.
 *
 * The recording, in the clock of the 48 kHz audio:
 *   - audio in blocks of 4800 samples that arrive 2000 samples late, starting
 *     7000 samples BEFORE the first frame (the part before the origin must be
 *     trimmed), a sawtooth tone and a 1 ms click at every exact second from the
 *     first frame; one block arrives short (a gap that becomes silence);
 *   - a frame every 4800 ticks with +-1500 of jitter and 3000 ticks of
 *     encoding latency; one in 37 is lost (it must become a repeat of the
 *     previous) and one in 53 arrives twice (the second must be dropped).
 *
 * Files live in a directory the case makes under /tmp; the next case (or the
 * end of the case) removes it. A case that fails leaves its directory until
 * the next one starts.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "unity.h"

#include "camera_drv_movie.h"

#define SPF         4800U               /* ticks of one video slot at 10 fps    */
#define R0          100000ULL           /* tick of the first frame              */
#define LAT_A       2000U               /* audio latency, in ticks              */
#define LAT_V       3000U               /* video encoding latency, in ticks     */
#define MAX_FRAMES  400U
#define FRAME_MAX   6000U
#define MAX_SIL     4U

static uint8_t  s_ring[1024U * 1024U];  /* 2 x 512 KB, as the muxer's notes advise */
static int16_t  s_audio[60000];         /* 1.25 s of audio                         */
static camera_drv_movie_t s_m;
static char     s_dir[64];

/* ---- A directory of the case's own ------------------------------------------------------- */

static void empty_dir_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;

    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char p[512];   /* a folder's path and a whole d_name (256) */
        if ((strcmp(de->d_name, ".") == 0) || (strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        int const w = snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if ((w < 0) || ((size_t)w >= sizeof p)) {
            continue;
        }
        (void)unlink(p);
    }
    (void)closedir(d);
}

/* The directory and what is in it, one level of folders down. */
static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;

    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char p[512];   /* a folder's path and a whole d_name (256) */
        struct stat st;
        if ((strcmp(de->d_name, ".") == 0) || (strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        int const w = snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if ((w < 0) || ((size_t)w >= sizeof p)) {
            continue;
        }
        if ((stat(p, &st) == 0) && S_ISDIR(st.st_mode)) {
            empty_dir_files(p);
            (void)rmdir(p);
        } else {
            (void)unlink(p);
        }
    }
    (void)closedir(d);
    (void)rmdir(dir);
}

static void tmp_drop(void)
{
    if (s_dir[0] != '\0') {
        remove_dir(s_dir);
        s_dir[0] = '\0';
    }
}

/** @brief A fresh directory for this case; the last case's is gone. */
static const char *tmp_new(void)
{
    tmp_drop();
    (void)snprintf(s_dir, sizeof s_dir, "/tmp/camera_drv_movie_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
    return s_dir;
}

static void path_in(char *out, size_t cap, const char *name)
{
    (void)snprintf(out, cap, "%s/%s", s_dir, name);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---- The signals fed in -------------------------------------------------------------------- */

/** @brief A frame: SOI, its id, noise from the id, EOI. Every id differs in bytes and mostly in size. */
static size_t frame_make(uint32_t id, uint8_t *out)
{
    size_t const len = 3000U + ((id * 131U) % 2500U);
    uint32_t x = (id * 2654435761U) + 12345U;

    out[0] = 0xFFU;
    out[1] = 0xD8U;
    out[2] = (uint8_t)id;
    out[3] = (uint8_t)(id >> 8);
    for (size_t i = 4U; i < (len - 2U); i++) {
        x = (x * 1664525U) + 1013904223U;
        out[i] = (uint8_t)(x >> 24);
    }
    out[len - 2U] = 0xFFU;
    out[len - 1U] = 0xD9U;
    return len;
}

/** @brief The audio at tick x: a sawtooth, and a 1 ms click at every exact second from R0. */
static int16_t tone(uint64_t x)
{
    if ((x >= R0) && (((x - R0) % 48000U) < 48U)) {
        return 30000;
    }
    return (int16_t)(((int32_t)(x % 109U) * 120) - 6500);
}

typedef struct {
    /* what to simulate */
    const char *path;
    double      seconds;
    bool        audio;
    bool        stamps;             /* false: every frame is CAMERA_DRV_MOVIE_NO_STAMP           */
    uint16_t    rotation;
    uint16_t    width;              /* 0 = 800                                          */
    uint16_t    height;             /* 0 = 600                                          */
    uint64_t    limit;              /* cfg.limit_bytes                                  */
    double      abort_at;           /* > 0: the power goes at this second               */
    uint32_t    pre_audio;          /* audio ticks before the first frame (0 = 7000)    */
    double      camera_from;           /* the camera gives no frames in [from, to) s       */
    double      camera_to;
    double      aud_from;           /* the audio is lost in [from, to) s                */
    double      aud_to;
    /* what happened */
    int         parts;
    uint32_t    nframes;            /* frames the loop went through, delivered or not   */
    bool        delivered[MAX_FRAMES];
    uint32_t    extras;             /* second frames sent for a slot already taken      */
    uint64_t    audio_end;          /* tick after the last audio sample handed over     */
    uint64_t    sil_lo[MAX_SIL];    /* ranges of the audio that never arrived           */
    uint64_t    sil_hi[MAX_SIL];
    uint32_t    nsil;
    camera_drv_movie_statistics_t stats;          /* just before the close (or the abort)             */
} sim_t;

static void silence_add(sim_t *s, uint64_t lo, uint64_t hi)
{
    if ((s->nsil != 0U) && (s->sil_hi[s->nsil - 1U] == lo)) {
        s->sil_hi[s->nsil - 1U] = hi;
        return;
    }
    TEST_ASSERT_TRUE(s->nsil < MAX_SIL);
    s->sil_lo[s->nsil] = lo;
    s->sil_hi[s->nsil] = hi;
    s->nsil++;
}

/** @brief The signal the file should hold at tick x. */
static int16_t expected_pcm(const sim_t *s, uint64_t x)
{
    for (uint32_t i = 0U; i < s->nsil; i++) {
        if ((x >= s->sil_lo[i]) && (x < s->sil_hi[i])) {
            return 0;
        }
    }
    return tone(x);
}

/** @brief Run the schedule. Fails the case on any answer the muxer should not give. */
static void sim_run(sim_t *c)
{
    static uint8_t jpeg[FRAME_MAX];
    static uint8_t other[FRAME_MAX];
    camera_drv_movie_configuration_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = (c->width != 0U) ? c->width : 800U;
    cfg.height = (c->height != 0U) ? c->height : 600U;
    cfg.fps = 10U;
    cfg.audio = c->audio;
    cfg.buf = s_ring;
    cfg.buf_bytes = sizeof s_ring;
    cfg.block_bytes = 512U * 1024U;
    cfg.audio_buf = s_audio;
    cfg.audio_samples = sizeof s_audio / sizeof s_audio[0];
    cfg.limit_bytes = c->limit;
    cfg.unix_time = 1790000000U;
    cfg.rotation = c->rotation;

    c->parts = 0;
    c->nframes = 0U;
    c->extras = 0U;
    c->nsil = 0U;
    memset(c->delivered, 0, sizeof c->delivered);
    camera_drv_movie_error_t e = camera_drv_movie_open(&s_m, c->path, &cfg);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)e);
    c->parts = 1;

    uint64_t const end = R0 + (uint64_t)(c->seconds * 48000);
    uint64_t const cut = (c->abort_at > 0) ? (R0 + (uint64_t)(c->abort_at * 48000)) : UINT64_MAX;
    uint64_t const a0 = R0 - ((c->pre_audio != 0U) ? c->pre_audio : 7000U);
    uint64_t next_a = a0;               /* the next audio block to hand over */
    bool gap_done = false;
    uint32_t i = 0U;                    /* the next frame */
    char part_path[CAMERA_DRV_MOVIE_PATH_MAX];
    int16_t pcm[4800];

    for (uint64_t t = a0; t < end; t += 100U) {
        if (t >= cut) {
            c->nframes = i;
            camera_drv_movie_statistics(&s_m, &c->stats);
            camera_drv_movie_abort(&s_m);
            return;
        }
        /* the audio whose block ended LAT_A ago */
        while (c->audio && ((next_a + 4800U + LAT_A) <= t)) {
            for (uint32_t k = 0U; k < 4800U; k++) {
                pcm[k] = tone(next_a + k);
            }
            bool const lost = (c->aud_to > 0) && (next_a >= (R0 + (uint64_t)(c->aud_from * 48000))) &&
                              (next_a < (R0 + (uint64_t)(c->aud_to * 48000)));
            if (lost) {
                silence_add(c, next_a, next_a + 4800U);
                e = CAMERA_DRV_MOVIE_OK;
            } else if (!gap_done && (next_a >= (R0 + 110000U))) {
                /* 1000 samples never arrive (no click inside) */
                silence_add(c, next_a, next_a + 1000U);
                e = camera_drv_movie_audio(&s_m, pcm + 1000, 3800U, next_a + 1000U);
                gap_done = true;
            } else {
                e = camera_drv_movie_audio(&s_m, pcm, 4800U, next_a);
            }
            TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)e);
            next_a += 4800U;
            c->audio_end = next_a;
        }
        /* the frames */
        for (;;) {
            int32_t const jit = (i == 0U) ? 0 : ((int32_t)((i * 7919U) % 3001U) - 1500);
            uint64_t tick = R0 + ((uint64_t)i * SPF) + (uint64_t)(int64_t)jit;
            if (!c->stamps) {
                tick = R0 + ((uint64_t)i * SPF);
            }
            if ((tick + LAT_V) > t) {
                break;
            }
            TEST_ASSERT_TRUE(i < MAX_FRAMES);
            bool lost = c->stamps && ((i % 37U) == 5U);
            if ((c->camera_to > 0) && (tick >= (R0 + (uint64_t)(c->camera_from * 48000))) &&
                (tick < (R0 + (uint64_t)(c->camera_to * 48000)))) {
                lost = true;
            }
            if (!lost) {
                size_t const len = frame_make(i, jpeg);
                e = camera_drv_movie_frame(&s_m, jpeg, len, c->stamps ? tick : CAMERA_DRV_MOVIE_NO_STAMP);
                TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)e);
                c->delivered[i] = true;
                if (c->stamps && ((i % 53U) == 7U)) {
                    /* the camera hands over another frame for the same slot: it is dropped */
                    size_t const len2 = frame_make(i + 1U, other);
                    e = camera_drv_movie_frame(&s_m, other, len2, tick + 1200U);
                    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_DROPPED, (int)e);
                    c->extras++;
                }
            }
            i++;
        }
        if (camera_drv_movie_full(&s_m)) {
            c->parts++;
            TEST_ASSERT_TRUE(camera_drv_movie_part_path(c->path, (uint32_t)c->parts, part_path, sizeof part_path));
            e = camera_drv_movie_split(&s_m, part_path);
            TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)e);
        }
    }
    c->nframes = i;
    camera_drv_movie_statistics(&s_m, &c->stats);
    e = camera_drv_movie_close(&s_m);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)e);
}

/** @brief The index of the last frame that was delivered. */
static uint32_t last_delivered(const sim_t *c)
{
    uint32_t last = 0U;
    for (uint32_t i = 0U; i < c->nframes; i++) {
        if (c->delivered[i]) {
            last = i;
        }
    }
    return last;
}

/** @brief Frames the camera lost with a later one delivered: each must be a repeat. */
static uint32_t lost_before_last(const sim_t *c)
{
    uint32_t const last = last_delivered(c);
    uint32_t n = 0U;
    for (uint32_t i = 0U; i < last; i++) {
        if (!c->delivered[i]) {
            n++;
        }
    }
    return n;
}

/* ---- A QuickTime reader that trusts nothing the muxer says --------------------------------- */

typedef struct {
    uint8_t  *d;
    size_t    len;
    size_t    mdat_end;
    bool      mdat64;
    size_t    moov_ini;             /* the first byte of the moov box                    */
    bool      co64;                 /* the video track uses 64-bit chunk offsets          */
    uint32_t  nv;                   /* video samples                                      */
    uint64_t *voff;
    uint32_t *vsize;
    uint32_t *vid;                  /* the frame id each sample holds                     */
    uint32_t  stts_entries;         /* of the video track                                 */
    uint32_t  stts_delta;
    bool      has_audio;
    size_t    na;                   /* audio samples                                      */
    int16_t  *pcm;
} qt_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** @brief The nth child box of type @p type in [ini, fin): its content range. */
static bool box_find(const uint8_t *d, size_t ini, size_t fin, const char *type, uint32_t nth, size_t *c_ini,
                     size_t *c_fin)
{
    while ((ini + 8U) <= fin) {
        uint64_t size = be32(d + ini);
        size_t head = 8U;
        if (size == 1U) {
            size = ((uint64_t)be32(d + ini + 8U) << 32) | be32(d + ini + 12U);
            head = 16U;
        } else if (size == 0U) {
            size = fin - ini;
        } else {
            /* a plain size */
        }
        if ((size < head) || ((ini + size) > fin)) {
            return false;
        }
        if (memcmp(d + ini + 4U, type, 4U) == 0) {
            if (nth == 0U) {
                *c_ini = ini + head;
                *c_fin = (size_t)(ini + size);
                return true;
            }
            nth--;
        }
        ini += (size_t)size;
    }
    return false;
}

/** @brief The stbl of the first track whose handler is @p kind ("vide" or "soun"). */
static bool find_stbl(const qt_t *q, const char *kind, size_t *s_ini, size_t *s_fin)
{
    size_t m_ini;
    size_t m_fin;
    if (!box_find(q->d, q->moov_ini, q->len, "moov", 0U, &m_ini, &m_fin)) {
        return false;
    }
    for (uint32_t nth = 0U; nth < 4U; nth++) {
        size_t t_ini;
        size_t t_fin;
        size_t d_ini;
        size_t d_fin;
        size_t h_ini;
        size_t h_fin;
        size_t n_ini;
        size_t n_fin;
        if (!box_find(q->d, m_ini, m_fin, "trak", nth, &t_ini, &t_fin) ||
            !box_find(q->d, t_ini, t_fin, "mdia", 0U, &d_ini, &d_fin) ||
            !box_find(q->d, d_ini, d_fin, "hdlr", 0U, &h_ini, &h_fin) || ((h_ini + 12U) > h_fin)) {
            return false;
        }
        if (memcmp(q->d + h_ini + 8U, kind, 4U) != 0) {
            continue;
        }
        return box_find(q->d, d_ini, d_fin, "minf", 0U, &n_ini, &n_fin) &&
               box_find(q->d, n_ini, n_fin, "stbl", 0U, s_ini, s_fin);
    }
    return false;
}

/** @brief Resolve one track's tables (stsz, stsc, stco or co64) into byte offsets. */
static void read_track(qt_t *q, const char *kind)
{
    size_t s_ini;
    size_t s_fin;
    size_t a;
    size_t b;
    TEST_ASSERT_TRUE(find_stbl(q, kind, &s_ini, &s_fin));
    bool const video = memcmp(kind, "vide", 4U) == 0;

    TEST_ASSERT_TRUE(box_find(q->d, s_ini, s_fin, "stsz", 0U, &a, &b));
    uint32_t const fixed = be32(q->d + a + 4U);
    uint32_t const count = be32(q->d + a + 8U);
    const uint8_t *sizes = q->d + a + 12U;
    if (fixed == 0U) {
        TEST_ASSERT_TRUE((a + 12U + (4U * (size_t)count)) <= b);
    }

    TEST_ASSERT_TRUE(box_find(q->d, s_ini, s_fin, "stsc", 0U, &a, &b));
    uint32_t const nruns = be32(q->d + a + 4U);
    const uint8_t *runs = q->d + a + 8U;
    TEST_ASSERT_TRUE((a + 8U + (12U * (size_t)nruns)) <= b);

    uint32_t nch;
    const uint8_t *offs;
    bool wide = false;
    if (box_find(q->d, s_ini, s_fin, "stco", 0U, &a, &b)) {
        nch = be32(q->d + a + 4U);
        offs = q->d + a + 8U;
        TEST_ASSERT_TRUE((a + 8U + (4U * (size_t)nch)) <= b);
    } else {
        TEST_ASSERT_TRUE(box_find(q->d, s_ini, s_fin, "co64", 0U, &a, &b));
        nch = be32(q->d + a + 4U);
        offs = q->d + a + 8U;
        wide = true;
        TEST_ASSERT_TRUE((a + 8U + (8U * (size_t)nch)) <= b);
    }
    if (video) {
        q->co64 = wide;
        q->nv = count;
        q->voff = calloc((size_t)count + 1U, sizeof(uint64_t));
        q->vsize = calloc((size_t)count + 1U, sizeof(uint32_t));
        q->vid = calloc((size_t)count + 1U, sizeof(uint32_t));
        TEST_ASSERT_NOT_NULL(q->voff);
        TEST_ASSERT_NOT_NULL(q->vsize);
        TEST_ASSERT_NOT_NULL(q->vid);
        TEST_ASSERT_TRUE(box_find(q->d, s_ini, s_fin, "stts", 0U, &a, &b));
        q->stts_entries = be32(q->d + a + 4U);
        q->stts_delta = be32(q->d + a + 12U);
    } else {
        TEST_ASSERT_EQUAL_UINT32(2U, fixed);          /* PCM: 2 bytes a sample, no table */
        q->has_audio = true;
        q->na = count;
        q->pcm = calloc((size_t)count + 1U, sizeof(int16_t));
        TEST_ASSERT_NOT_NULL(q->pcm);
    }

    uint32_t k = 0U;                                  /* samples placed so far */
    for (uint32_t c = 0U; c < nch; c++) {
        uint64_t off = wide ? (((uint64_t)be32(offs + (8U * c)) << 32) | be32(offs + (8U * c) + 4U))
                            : be32(offs + (4U * c));
        uint32_t spc = 0U;                            /* the run in force for chunk c + 1 */
        for (uint32_t r = 0U; r < nruns; r++) {
            if (be32(runs + (12U * r)) <= (c + 1U)) {
                spc = be32(runs + (12U * r) + 4U);
            }
        }
        TEST_ASSERT_TRUE(spc != 0U);
        TEST_ASSERT_TRUE((k + spc) <= count);
        if (video) {
            for (uint32_t s = 0U; s < spc; s++) {
                uint32_t const size = (fixed != 0U) ? fixed : be32(sizes + (4U * (k + s)));
                q->voff[k + s] = off;
                q->vsize[k + s] = size;
                off += size;
            }
        } else {
            TEST_ASSERT_TRUE((off + (2U * (uint64_t)spc)) <= q->mdat_end);
            memcpy(&q->pcm[k], q->d + off, 2U * (size_t)spc);
        }
        k += spc;
    }
    TEST_ASSERT_EQUAL_UINT32(count, k);
}

/** @brief Open a closed or repaired file and resolve both tracks. */
static void qt_load(const char *path, qt_t *q)
{
    memset(q, 0, sizeof *q);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long const size = ftell(f);
    TEST_ASSERT_TRUE(size > 100);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
    q->len = (size_t)size;
    q->d = malloc(q->len);
    TEST_ASSERT_NOT_NULL(q->d);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)q->len, (uint32_t)fread(q->d, 1U, q->len, f));
    (void)fclose(f);

    /* ftyp 'qt  ', then wide + mdat (or a 64-bit mdat), then the moov to the very end */
    TEST_ASSERT_EQUAL_UINT32(20U, be32(q->d));
    TEST_ASSERT_EQUAL_INT(0, memcmp(q->d + 4, "ftypqt  ", 8U));
    if (be32(q->d + 20) == 8U) {
        TEST_ASSERT_EQUAL_INT(0, memcmp(q->d + 24, "wide", 4U));
        TEST_ASSERT_EQUAL_INT(0, memcmp(q->d + 32, "mdat", 4U));
        q->mdat_end = 28U + (size_t)be32(q->d + 28);
    } else {
        TEST_ASSERT_EQUAL_UINT32(1U, be32(q->d + 20));
        TEST_ASSERT_EQUAL_INT(0, memcmp(q->d + 24, "mdat", 4U));
        q->mdat64 = true;
        q->mdat_end = 20U + (size_t)(((uint64_t)be32(q->d + 28) << 32) | be32(q->d + 32));
    }
    TEST_ASSERT_TRUE((q->mdat_end + 8U) < q->len);
    q->moov_ini = q->mdat_end;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(q->len - q->moov_ini), be32(q->d + q->moov_ini));
    TEST_ASSERT_EQUAL_INT(0, memcmp(q->d + q->moov_ini + 4U, "moov", 4U));

    read_track(q, "vide");
    size_t s_ini;
    size_t s_fin;
    if (find_stbl(q, "soun", &s_ini, &s_fin)) {
        read_track(q, "soun");
    }
    /* every video sample is one whole frame inside mdat */
    for (uint32_t n = 0U; n < q->nv; n++) {
        static uint8_t want[FRAME_MAX];
        TEST_ASSERT_TRUE(q->voff[n] >= 36U);
        TEST_ASSERT_TRUE((q->voff[n] + q->vsize[n]) <= q->mdat_end);
        const uint8_t *s = q->d + q->voff[n];
        TEST_ASSERT_TRUE(q->vsize[n] > 4U);
        uint32_t const id = (uint32_t)s[2] | ((uint32_t)s[3] << 8);
        size_t const len = frame_make(id, want);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)len, q->vsize[n]);
        TEST_ASSERT_EQUAL_INT(0, memcmp(want, s, len));
        q->vid[n] = id;
    }
}

static void qt_free(qt_t *q)
{
    free(q->d);
    free(q->voff);
    free(q->vsize);
    free(q->vid);
    free(q->pcm);
    memset(q, 0, sizeof *q);
}

/**
 * @brief Check the video of a file against the schedule.
 *
 * @param first_slot The global slot the file's sample 0 stands for (0 for a
 *                   file, the audio so far over 4800 for a later part).
 * @param part       A later part: its first samples may show a frame 1-2 slots
 *                   early (the muxer's notes say so), so only from the fourth
 *                   sample on is the slot exact.
 */
static void check_video(const sim_t *c, const qt_t *q, uint32_t first_slot, bool part)
{
    uint32_t prev = 0U;

    for (uint32_t n = 0U; n < q->nv; n++) {
        uint32_t const g = first_slot + n;
        uint32_t const id = q->vid[n];
        if (part && (n < 3U)) {
            TEST_ASSERT_TRUE((id + 1U) >= g);
            TEST_ASSERT_TRUE(id <= (g + 2U));
        } else {
            uint32_t const want = ((g < c->nframes) && c->delivered[g]) ? g : prev;
            TEST_ASSERT_EQUAL_UINT32(want, id);
        }
        prev = id;
    }
}

/** @brief Check a file's audio, sample by sample, against the signal fed in. */
static void check_audio(const sim_t *c, const qt_t *q, uint64_t before)
{
    for (size_t n = 0U; n < q->na; n++) {
        int16_t const want = expected_pcm(c, R0 + before + n);
        if (q->pcm[n] != want) {
            char msg[64];
            (void)snprintf(msg, sizeof msg, "audio sample %lu", (unsigned long)n);
            TEST_ASSERT_EQUAL_INT16_MESSAGE(want, q->pcm[n], msg);
        }
    }
}

/* ---- A run of the first schedule, the one most cases look at ------------------------------------ */

static void run_with_audio(sim_t *c, const char *name)
{
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    (void)tmp_new();
    path_in(path, sizeof path, name);
    c->path = path;
    c->seconds = 20;
    c->audio = true;
    c->stamps = true;
    sim_run(c);
}

/* ---- The cases --------------------------------------------------------------------------------------- */

TEST_CASE("camera_drv_movie: a recording with audio closes into ftyp, wide, mdat, moov and no journal", "[mov]")
{
    static sim_t sim;
    memset(&sim, 0, sizeof sim);
    run_with_audio(&sim, "bien.mov");

    char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(sim.path, journal, sizeof journal));
    TEST_ASSERT_FALSE(exists(journal));

    qt_t q;
    qt_load(sim.path, &q);                       /* the layout is checked in here */
    TEST_ASSERT_FALSE(q.mdat64);
    TEST_ASSERT_FALSE(q.co64);

    camera_drv_movie_info_t in;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(sim.path, &in));
    TEST_ASSERT_EQUAL_UINT16(800U, in.width);
    TEST_ASSERT_EQUAL_UINT16(600U, in.height);
    TEST_ASSERT_EQUAL_UINT16(0U, in.rotation);
    TEST_ASSERT_EQUAL_UINT16(10U, in.fps);
    TEST_ASSERT_TRUE(in.audio);
    TEST_ASSERT_EQUAL_UINT32(48000U, in.audio_hz);
    TEST_ASSERT_EQUAL_UINT32(q.nv, in.slots);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)q.na, (uint32_t)in.audio_samples);
    /* the numbers this schedule gives */
    TEST_ASSERT_EQUAL_UINT32(200U, in.slots);
    TEST_ASSERT_EQUAL_UINT32(957800U, (uint32_t)in.audio_samples);
    TEST_ASSERT_EQUAL_UINT32(20000U, in.ms);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: frames go to the slot of their tick; a late one is dropped, a lost one repeats the previous", "[mov]")
{
    static sim_t sim;
    memset(&sim, 0, sizeof sim);
    run_with_audio(&sim, "bien.mov");

    qt_t q;
    qt_load(sim.path, &q);
    TEST_ASSERT_EQUAL_UINT32(1U, q.stts_entries);   /* one entry: every sample lasts 1/fps */
    TEST_ASSERT_EQUAL_UINT32(SPF, q.stts_delta);
    check_video(&sim, &q, 0U, false);
    TEST_ASSERT_EQUAL_UINT32(sim.extras, sim.stats.dropped);
    TEST_ASSERT_EQUAL_UINT32(4U, sim.extras);
    TEST_ASSERT_EQUAL_UINT32(lost_before_last(&sim), sim.stats.duplicates);
    TEST_ASSERT_EQUAL_UINT32(6U, sim.stats.duplicates);
    /* the repeats copy no bytes: the same offset twice */
    uint32_t same = 0U;
    for (uint32_t n = 1U; n < q.nv; n++) {
        if (q.voff[n] == q.voff[n - 1U]) {
            same++;
        }
    }
    TEST_ASSERT_TRUE(same >= sim.stats.duplicates);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: the audio is the clock: trimmed to the origin, silence in the gap, no sample lost", "[mov]")
{
    static sim_t sim;
    memset(&sim, 0, sizeof sim);
    run_with_audio(&sim, "bien.mov");

    qt_t q;
    qt_load(sim.path, &q);
    TEST_ASSERT_TRUE(q.has_audio);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sim.audio_end - R0), (uint32_t)q.na);
    check_audio(&sim, &q, 0U);
    TEST_ASSERT_EQUAL_UINT32(1000U, (uint32_t)sim.stats.silence);
    TEST_ASSERT_EQUAL_UINT32(7000U, (uint32_t)sim.stats.audio_dropped);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: with no audio and no stamps every frame takes the next slot", "[mov]")
{
    static sim_t sim;
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(path, sizeof path, "sin_audio.mov");
    sim.path = path;
    sim.seconds = 6;
    sim.audio = false;
    sim.stamps = false;
    sim_run(&sim);

    qt_t q;
    qt_load(path, &q);
    TEST_ASSERT_FALSE(q.has_audio);
    TEST_ASSERT_EQUAL_UINT32(60U, q.nv);
    check_video(&sim, &q, 0U, false);
    camera_drv_movie_info_t in;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    TEST_ASSERT_FALSE(in.audio);
    TEST_ASSERT_EQUAL_UINT32(60U, in.slots);
    TEST_ASSERT_EQUAL_UINT32(6000U, in.ms);
    qt_free(&q);
    tmp_drop();
}

static void check_rotation(uint16_t rotation, const char *name)
{
    static sim_t sim;
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(path, sizeof path, name);
    sim.path = path;
    sim.seconds = 3;
    sim.audio = false;
    sim.stamps = false;
    sim.rotation = rotation;
    sim_run(&sim);

    camera_drv_movie_info_t in;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    TEST_ASSERT_EQUAL_UINT16(rotation, in.rotation);
    TEST_ASSERT_EQUAL_UINT16(800U, in.width);          /* tkhd keeps the encoded size */
    TEST_ASSERT_EQUAL_UINT16(600U, in.height);
    TEST_ASSERT_EQUAL_UINT32(30U, in.slots);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: a rotation of 90 degrees goes into tkhd's matrix and reads back", "[mov]")
{
    check_rotation(90U, "girado90.mov");
}

TEST_CASE("camera_drv_movie: a rotation of 270 degrees goes into tkhd's matrix and reads back", "[mov]")
{
    check_rotation(270U, "girado270.mov");
}

/** @brief The end of the data the journal covers (its format is in camera_drv_movie.c). */
static uint64_t journal_data_end(const char *journal, uint32_t *records)
{
    FILE *f = fopen(journal, "rb");
    uint8_t b[600];
    uint64_t end = 0U;

    *records = 0U;
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 64, SEEK_SET));
    while (fread(b, 1U, 32U, f) == 32U) {
        uint32_t magic;
        uint32_t nfr;
        uint32_t nau;
        uint64_t ov;
        uint64_t oa;
        memcpy(&magic, b, 4U);
        memcpy(&nfr, b + 4, 4U);
        memcpy(&nau, b + 8, 4U);
        memcpy(&ov, b + 16, 8U);
        memcpy(&oa, b + 24, 8U);
        if ((magic != 0x31514C42U) || (nfr > 64U)) {
            break;
        }
        if (fread(b + 32, 1U, (8U * (size_t)nfr) + 4U, f) != ((8U * (size_t)nfr) + 4U)) {
            break;
        }
        uint64_t tv = 0U;
        for (uint32_t j = 0U; j < nfr; j++) {
            uint32_t t;
            memcpy(&t, b + 32 + (8U * j), 4U);
            tv += t;
        }
        uint64_t const fv = ov + tv;
        uint64_t const fa = oa + (2U * (uint64_t)nau);
        uint64_t const f1 = (fv > fa) ? fv : fa;
        if (f1 > end) {
            end = f1;
        }
        (*records)++;
    }
    (void)fclose(f);
    return end;
}

static long file_len(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : -1L;
}

TEST_CASE("camera_drv_movie: a power cut leaves a file that repair_dir rebuilds up to the last whole block", "[mov]")
{
    static sim_t sim;
    static char dir2[CAMERA_DRV_MOVIE_PATH_MAX];
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(dir2, sizeof dir2, "corte");
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir2, 0755));
    path_in(path, sizeof path, "corte/VID_corte.mov");
    sim.path = path;
    sim.seconds = 20;
    sim.audio = true;
    sim.stamps = true;
    sim.rotation = 90U;
    sim.abort_at = 9.5;
    sim_run(&sim);

    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));
    uint32_t nrec;
    uint64_t const end = journal_data_end(journal, &nrec);
    TEST_ASSERT_EQUAL_UINT32(9U, nrec);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)end, (uint32_t)file_len(path));

    /* what a cut leaves besides: bytes that were never indexed, and half a journal record */
    FILE *f = fopen(path, "ab");
    TEST_ASSERT_NOT_NULL(f);
    for (uint32_t k = 0U; k < 5000U; k++) {
        (void)fputc(0xA5, f);
    }
    (void)fclose(f);
    f = fopen(journal, "ab");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t const half[20] = { 0x42, 0x4C, 0x51, 0x31, 3, 0, 0, 0, 0x80, 0xBB };
    TEST_ASSERT_EQUAL_UINT32(sizeof half, (uint32_t)fwrite(half, 1U, sizeof half, f));
    (void)fclose(f);

    camera_drv_movie_info_t in;
    TEST_ASSERT_TRUE(camera_drv_movie_info(path, &in) != CAMERA_DRV_MOVIE_OK);       /* no moov yet */

    TEST_ASSERT_EQUAL_INT(1, camera_drv_movie_repair_directory(dir2, NULL, 0U));
    TEST_ASSERT_FALSE(exists(journal));

    /* the numbers this schedule gives: 9 blocks, 91 slots, 432000 samples */
    qt_t q;
    qt_load(path, &q);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)end, (uint32_t)q.mdat_end);   /* the 5000 stray bytes are gone */
    TEST_ASSERT_EQUAL_UINT32(91U, q.nv);
    TEST_ASSERT_EQUAL_UINT32(432000U, (uint32_t)q.na);
    check_video(&sim, &q, 0U, false);
    check_audio(&sim, &q, 0U);
    qt_free(&q);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    TEST_ASSERT_EQUAL_UINT16(90U, in.rotation);                      /* the header survived the cut */
    TEST_ASSERT_EQUAL_UINT32(91U, in.slots);
    TEST_ASSERT_EQUAL_UINT32(9100U, in.ms);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: repair_dir mends every journal of a folder, however many, and a missing folder is an error", "[mov]")
{
    enum { MOVIES = 21 };   /* more than the 16 names a pass holds */
    static sim_t sim;
    static char dir2[CAMERA_DRV_MOVIE_PATH_MAX];
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];

    (void)tmp_new();
    path_in(dir2, sizeof dir2, "muchos");
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir2, 0755));
    for (int k = 0; k < MOVIES; k++) {
        char name[32];
        (void)snprintf(name, sizeof name, "muchos/VID_%02d.mov", k);
        path_in(path, sizeof path, name);
        memset(&sim, 0, sizeof sim);
        sim.path = path;
        sim.seconds = 8;
        sim.abort_at = 4.5;
        sim_run(&sim);
        TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));
        TEST_ASSERT_TRUE(exists(journal));
    }
    TEST_ASSERT_EQUAL_INT(MOVIES, camera_drv_movie_repair_directory(dir2, NULL, 0U));
    for (int k = 0; k < MOVIES; k++) {
        char name[32];
        camera_drv_movie_info_t in;
        (void)snprintf(name, sizeof name, "muchos/VID_%02d.mov", k);
        path_in(path, sizeof path, name);
        TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));
        TEST_ASSERT_FALSE(exists(journal));
        TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    }
    TEST_ASSERT_EQUAL_INT(0, camera_drv_movie_repair_directory(dir2, NULL, 0U));   /* nothing left to do */
    path_in(path, sizeof path, "no_hay");
    TEST_ASSERT_TRUE(camera_drv_movie_repair_directory(path, NULL, 0U) < 0);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: a file that was closed properly has no journal and nothing to repair", "[mov]")
{
    static sim_t sim;
    memset(&sim, 0, sizeof sim);
    run_with_audio(&sim, "bien.mov");
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_NO_JOURNAL, (int)camera_drv_movie_repair(sim.path, NULL, 0U, NULL));
    tmp_drop();
}

TEST_CASE("camera_drv_movie: when the last block was never fully written, repair keeps the blocks before it", "[mov]")
{
    static sim_t sim;
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    path_in(path, sizeof path, "corte2.mov");
    sim.path = path;
    sim.seconds = 20;
    sim.audio = true;
    sim.stamps = true;
    sim.abort_at = 9.5;
    sim_run(&sim);

    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));
    uint32_t nrec;
    uint64_t const end = journal_data_end(journal, &nrec);
    TEST_ASSERT_EQUAL_INT(0, truncate(path, (off_t)(end - 1U)));    /* FAT never kept the last byte */

    camera_drv_movie_repair_t info;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_repair(path, s_ring, sizeof s_ring, &info));
    TEST_ASSERT_EQUAL_UINT32(nrec - 1U, info.blocks);
    TEST_ASSERT_EQUAL_UINT32(8U, info.blocks);                       /* this schedule's numbers */
    TEST_ASSERT_EQUAL_UINT32(81U, info.slots);
    TEST_ASSERT_EQUAL_UINT32(384000U, (uint32_t)info.audio_samples);
    TEST_ASSERT_EQUAL_UINT32(8100U, info.ms);
    TEST_ASSERT_FALSE(exists(journal));

    qt_t q;
    qt_load(path, &q);
    TEST_ASSERT_EQUAL_UINT32(info.slots, q.nv);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)info.audio_samples, (uint32_t)q.na);
    check_video(&sim, &q, 0U, false);
    check_audio(&sim, &q, 0U);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: at the size limit the file is split with no audio sample lost", "[mov]")
{
    static sim_t sim;
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char part[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(path, sizeof path, "partido.mov");
    sim.path = path;
    sim.seconds = 30;
    sim.audio = true;
    sim.stamps = true;
    sim.limit = 2500000U;               /* the 3.9 GB of the card, shrunk to a few seconds */
    sim_run(&sim);
    TEST_ASSERT_TRUE(sim.parts >= 3);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sim.parts - 1U, sim.stats.parts);

    uint64_t before = 0U;
    for (int p = 1; p <= sim.parts; p++) {
        if (p == 1) {
            (void)snprintf(part, sizeof part, "%s", path);
        } else {
            TEST_ASSERT_TRUE(camera_drv_movie_part_path(path, (uint32_t)p, part, sizeof part));
        }
        TEST_ASSERT_TRUE(camera_drv_movie_journal_path(part, journal, sizeof journal));
        TEST_ASSERT_FALSE(exists(journal));
        qt_t q;
        qt_load(part, &q);
        TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)(before % SPF));      /* every cut is on the video grid */
        check_video(&sim, &q, (uint32_t)(before / SPF), p > 1);
        check_audio(&sim, &q, before);
        camera_drv_movie_info_t in;
        TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(part, &in));
        TEST_ASSERT_EQUAL_UINT32(q.nv, in.slots);
        before += q.na;
        qt_free(&q);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sim.audio_end - R0), (uint32_t)before);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: a stopped camera repeats the last frame and lost audio becomes silence", "[mov]")
{
    static sim_t sim;
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(path, sizeof path, "pausas.mov");
    sim.path = path;
    sim.seconds = 20;
    sim.audio = true;
    sim.stamps = true;
    sim.pre_audio = 72000U;             /* 1.5 s of audio before the first frame: more than the buffer holds */
    sim.camera_from = 5;
    sim.camera_to = 9;
    sim.aud_from = 12;
    sim.aud_to = 14.5;
    sim_run(&sim);

    qt_t q;
    qt_load(path, &q);
    TEST_ASSERT_EQUAL_UINT32(200U, q.nv);                            /* this schedule's numbers */
    TEST_ASSERT_EQUAL_UINT32(955200U, (uint32_t)q.na);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sim.audio_end - R0), (uint32_t)q.na);
    check_video(&sim, &q, 0U, false);
    check_audio(&sim, &q, 0U);
    TEST_ASSERT_EQUAL_UINT32(lost_before_last(&sim), sim.stats.duplicates);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: a vertical 448x800 recording keeps its size and needs no rotation", "[mov]")
{
    static sim_t sim;
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    memset(&sim, 0, sizeof sim);
    (void)tmp_new();
    path_in(path, sizeof path, "vertical.mov");
    sim.path = path;
    sim.seconds = 8;
    sim.audio = true;
    sim.stamps = true;
    sim.width = 448U;
    sim.height = 800U;
    sim_run(&sim);

    camera_drv_movie_info_t in;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    TEST_ASSERT_EQUAL_UINT16(448U, in.width);
    TEST_ASSERT_EQUAL_UINT16(800U, in.height);
    TEST_ASSERT_EQUAL_UINT16(0U, in.rotation);
    TEST_ASSERT_EQUAL_UINT32(80U, in.slots);                         /* this schedule's numbers */
    TEST_ASSERT_EQUAL_UINT32(381800U, (uint32_t)in.audio_samples);
    qt_t q;
    qt_load(path, &q);
    check_video(&sim, &q, 0U, false);
    check_audio(&sim, &q, 0U);
    qt_free(&q);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: closing with no frame deletes the file, and an fps that does not divide 48000 is refused", "[mov]")
{
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    (void)tmp_new();
    path_in(path, sizeof path, "vacio.mov");
    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));

    camera_drv_movie_configuration_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = 800U;
    cfg.height = 600U;
    cfg.fps = 10U;
    cfg.buf = s_ring;
    cfg.buf_bytes = sizeof s_ring;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_open(&s_m, path, &cfg));
    TEST_ASSERT_TRUE(exists(path));
    TEST_ASSERT_TRUE(exists(journal));
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_EMPTY, (int)camera_drv_movie_close(&s_m));
    TEST_ASSERT_FALSE(exists(path));
    TEST_ASSERT_FALSE(exists(journal));

    cfg.fps = 7U;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_ARGUMENT, (int)camera_drv_movie_open(&s_m, path, &cfg));
    TEST_ASSERT_FALSE(exists(path));
    tmp_drop();
}

/* ---- Past 4 GB, a failed open, and the header reader ------------------------------------------------ */

static uint32_t crc32_bits(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFU;

    for (size_t i = 0U; i < n; i++) {
        c ^= p[i];
        for (uint32_t b = 0U; b < 8U; b++) {
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
        }
    }
    return ~c;
}

static void le32_put(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(v >> (8U * i));
    }
}

static void le64_put(uint8_t *p, uint64_t v)
{
    le32_put(p, (uint32_t)v);
    le32_put(p + 4, (uint32_t)(v >> 32));
}

/** @brief Read @p n bytes at @p off of a file that may be bigger than 4 GB. */
static void read_at(const char *path, uint64_t off, void *out, size_t n)
{
    int const fd = open(path, O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_TRUE(lseek(fd, (off_t)off, SEEK_SET) == (off_t)off);
    TEST_ASSERT_TRUE(read(fd, out, n) == (ssize_t)n);
    (void)close(fd);
}

TEST_CASE("camera_drv_movie: past 4 GB repair rewrites wide+mdat as one 64-bit mdat and uses co64", "[mov]")
{
    /* The journal of a recording of three frames of 1.5 GB is made by hand, over a sparse .mov
     * of the right length: the muxer reads the journal and the file's size and never the frames. */
    enum { FRAMES = 3 };
    uint64_t const frame = 1500000000ULL;
    uint64_t const data_end = 36U + (FRAMES * frame);
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];

    if (sizeof(off_t) < 8U) {
        TEST_IGNORE_MESSAGE("needs a 64-bit off_t");
    }
    (void)tmp_new();
    path_in(path, sizeof path, "grande.mov");
    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));

    uint8_t head[64];
    memset(head, 0, sizeof head);
    memcpy(head, "KMOVIDX1", 8U);
    le32_put(head + 8, 1U);
    head[12] = (uint8_t)(800U & 0xFFU);
    head[13] = (uint8_t)(800U >> 8);
    head[14] = (uint8_t)(600U & 0xFFU);
    head[15] = (uint8_t)(600U >> 8);
    head[16] = 10U;
    le64_put(head + 24, 20U);
    le32_put(head + 60, crc32_bits(head, 60U));
    uint8_t rec[32U + (8U * FRAMES) + 4U];
    memset(rec, 0, sizeof rec);
    le32_put(rec, 0x31514C42U);
    le32_put(rec + 4, FRAMES);
    le64_put(rec + 16, 36U);
    for (uint32_t j = 0U; j < FRAMES; j++) {
        le32_put(rec + 32 + (8U * j), (uint32_t)frame);
    }
    le32_put(rec + sizeof rec - 4U, crc32_bits(rec, sizeof rec - 4U));

    FILE *f = fopen(journal, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(sizeof head, (uint32_t)fwrite(head, 1U, sizeof head, f));
    TEST_ASSERT_EQUAL_UINT32(sizeof rec, (uint32_t)fwrite(rec, 1U, sizeof rec, f));
    (void)fclose(f);
    uint8_t const init[36] = {
        0, 0, 0, 20, 'f', 't', 'y', 'p', 'q', 't', ' ', ' ', 0, 0, 2, 0, 'q', 't', ' ', ' ',
        0, 0, 0, 8, 'w', 'i', 'd', 'e', 0, 0, 0, 0, 'm', 'd', 'a', 't',
    };
    int const fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_TRUE(write(fd, init, sizeof init) == (ssize_t)sizeof init);
    if (ftruncate(fd, (off_t)data_end) != 0) {
        (void)close(fd);
        TEST_IGNORE_MESSAGE("this filesystem cannot make a 4.5 GB sparse file");
    }
    (void)close(fd);

    camera_drv_movie_repair_t info;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_repair(path, s_ring, sizeof s_ring, &info));
    TEST_ASSERT_EQUAL_UINT32(FRAMES, info.slots);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)info.ignored_bytes);
    TEST_ASSERT_FALSE(exists(journal));

    uint8_t h[16];
    read_at(path, 20U, h, sizeof h);                 /* wide + mdat became size 1, 'mdat', largesize */
    TEST_ASSERT_EQUAL_UINT32(1U, be32(h));
    TEST_ASSERT_EQUAL_INT(0, memcmp(h + 4, "mdat", 4U));
    TEST_ASSERT_TRUE((((uint64_t)be32(h + 8) << 32) | be32(h + 12)) == (data_end - 20U));

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    uint64_t const moov_size = (uint64_t)st.st_size - data_end;   /* the moov starts where the data ends */
    TEST_ASSERT_TRUE((moov_size > 100U) && (moov_size < 4096U));
    uint8_t moov[4096];
    read_at(path, data_end, moov, (size_t)moov_size);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)moov_size, be32(moov));
    TEST_ASSERT_EQUAL_INT(0, memcmp(moov + 4, "moov", 4U));
    bool co64 = false;
    bool stco = false;
    for (size_t i = 0U; (i + 4U) <= (size_t)moov_size; i++) {
        co64 = co64 || (memcmp(moov + i, "co64", 4U) == 0);
        stco = stco || (memcmp(moov + i, "stco", 4U) == 0);
    }
    TEST_ASSERT_TRUE(co64);
    TEST_ASSERT_FALSE(stco);

    camera_drv_movie_info_t in;                               /* the header reader follows a 64-bit box too */
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_OK, (int)camera_drv_movie_info(path, &in));
    TEST_ASSERT_EQUAL_UINT32(FRAMES, in.slots);
    TEST_ASSERT_EQUAL_UINT16(800U, in.width);
    TEST_ASSERT_EQUAL_UINT16(600U, in.height);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: a failed open closes what it opened, so abort has nothing left to close twice", "[mov]")
{
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    static char journal[CAMERA_DRV_MOVIE_PATH_MAX + 8U];
    (void)tmp_new();
    path_in(path, sizeof path, "malo.mov");
    TEST_ASSERT_TRUE(camera_drv_movie_journal_path(path, journal, sizeof journal));
    TEST_ASSERT_EQUAL_INT(0, mkdir(journal, 0755));                 /* the journal cannot be created */

    camera_drv_movie_configuration_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = 800U;
    cfg.height = 600U;
    cfg.fps = 10U;
    cfg.buf = s_ring;
    cfg.buf_bytes = sizeof s_ring;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_IO, (int)camera_drv_movie_open(&s_m, path, &cfg));
    TEST_ASSERT_EQUAL_INT(-1, s_m.fd);                              /* a peek: tests may */
    TEST_ASSERT_EQUAL_INT(-1, s_m.jfd);
    TEST_ASSERT_FALSE(exists(path));
    camera_drv_movie_abort(&s_m);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_STATE, (int)camera_drv_movie_close(&s_m));
    tmp_drop();
}

TEST_CASE("camera_drv_movie: the header reader reads what a short version-1 mvhd does not say as zero", "[mov]")
{
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    (void)tmp_new();
    path_in(path, sizeof path, "corto.mov");
    /* moov{ mvhd } where mvhd (version 1) ends 4 bytes before the duration field it should hold */
    uint8_t m[40];
    memset(m, 0, sizeof m);
    m[3] = 40U;
    memcpy(m + 4, "moov", 4U);
    m[11] = 32U;
    memcpy(m + 12, "mvhd", 4U);
    m[16] = 1U;                                      /* version 1 */
    m[16 + 22] = 0x03U;                              /* timescale 1000, at 20..23 of the content */
    m[16 + 23] = 0xE8U;
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(sizeof m, (uint32_t)fwrite(m, 1U, sizeof m, f));
    (void)fclose(f);

    camera_drv_movie_info_t in;
    memset(&in, 0xAB, sizeof in);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_JOURNAL, (int)camera_drv_movie_info(path, &in));   /* no video track */
    TEST_ASSERT_EQUAL_UINT32(0U, in.ms);
    tmp_drop();
}

TEST_CASE("camera_drv_movie: the header reader answers a missing file with I/O and a non-movie with journal", "[mov]")
{
    static char path[CAMERA_DRV_MOVIE_PATH_MAX];
    (void)tmp_new();
    path_in(path, sizeof path, "no_existe.mov");
    camera_drv_movie_info_t in;
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_IO, (int)camera_drv_movie_info(path, &in));

    uint8_t junk[1000];
    memset(junk, 0xAB, sizeof junk);
    junk[0] = 0xFFU;                                 /* a box that claims more than the file has */
    path_in(path, sizeof path, "basura.mov");
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(sizeof junk, (uint32_t)fwrite(junk, 1U, sizeof junk, f));
    (void)fclose(f);
    TEST_ASSERT_EQUAL_INT(CAMERA_DRV_MOVIE_ERROR_JOURNAL, (int)camera_drv_movie_info(path, &in));
    tmp_drop();
}
