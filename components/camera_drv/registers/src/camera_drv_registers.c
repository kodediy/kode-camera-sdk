/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_registers.c
 * @brief The registers this SDK may touch, and the ones it may not. See
 *        camera_drv_registers.h.
 */
#include "camera_drv_registers.h"

#include "camera_bsp.h"

/* ---- The locked registers --------------------------------------------------
 *
 * Inclusive ranges. A register here sets a clock, a divider, the frame's
 * timing, the output pads or the format. */
typedef struct {
    uint16_t first;
    uint16_t last;
} span_t;

static const span_t LOCKED[] = {
    { 0x3000U, 0x3008U },  /* resets, clock enables, software power-down     */
    { 0x3016U, 0x301EU },  /* pad output enables, values and selects          */
    { 0x302CU, 0x302CU },  /* pad drive strength (reset value 1x)             */
    { 0x3034U, 0x3039U },  /* the PLL                                         */
    { 0x3103U, 0x3103U },  /* the system clock's source                       */
    { 0x3108U, 0x3108U },  /* the clock root dividers                         */
    { 0x3800U, 0x3821U },  /* window, HTS/VTS, offsets, scaling, flip, binning */
    { 0x3824U, 0x3824U },  /* the DVP pixel clock divider                     */
    { 0x3A00U, 0x3A0EU },  /* AEC control (night mode), max exposure, bands   */
    { 0x3A14U, 0x3A15U },  /* max exposure at 50 Hz                           */
    { 0x4202U, 0x4202U },  /* frame control: the stream                       */
    { 0x4300U, 0x4300U },  /* the output format                               */
    { 0x4407U, 0x4407U },  /* JPEG control                                    */
    { 0x460BU, 0x460CU },  /* VFIFO and its pixel clock divider               */
    { 0x4713U, 0x4713U },  /* JPEG mode                                       */
    { 0x4740U, 0x4740U },  /* PCLK, HREF and VSYNC polarities                 */
    { 0x4837U, 0x4837U },  /* the pixel clock period                          */
    { 0x501FU, 0x501FU },  /* the format mux                                  */
};

bool camera_drv_registers_locked(uint16_t reg)
{
    for (size_t i = 0U; i < (sizeof(LOCKED) / sizeof(LOCKED[0])); i++) {
        if ((reg >= LOCKED[i].first) && (reg <= LOCKED[i].last)) {
            return true;
        }
    }
    return false;
}

bool camera_drv_registers_writable(uint16_t reg, uint8_t mask)
{
    if (!camera_drv_registers_locked(reg)) {
        return true;
    }
    /* The autofocus microcontroller's reset: it clocks nothing on the pads. */
    return (reg == CAMERA_DRV_REGISTERS_ADDRESS_SYS_RESET00) && (mask == CAMERA_DRV_REGISTERS_MICROCONTROLLER_RESET_BIT);
}

/* ---- The controls ------------------------------------------------------------ */

#define EXPOSURE_MAX ((int32_t)((CAMERA_BSP_VTS - 4U) * 16U))

/* The format table's AEC gain ceiling, 0x3A18/0x3A19 = 0x0078: 7.5x. */
#define CEILING_TABLE 0x78

static const camera_drv_registers_control_info_t INFO[CAMERA_DRV_REGISTERS_CONTROL_COUNT] = {
    [CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN]     = { "test_pattern", 0, 1, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL]        = { "ae_manual", 0, 1, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL]        = { "ag_manual", 0, 1, 0, true, false },
    /* The neutral loop starts from the sensor's own balance: nothing to write. */
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE]               = { "wb", 0, (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_COUNT - 1,
                                    (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE]         = { "exposure", 0, EXPOSURE_MAX, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN]             = { "gain", 0, 1023, 16, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED]          = { "awb_red", 0, 4095, 1024, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN]        = { "awb_green", 0, 4095, 1024, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE]         = { "awb_blue", 0, 4095, 1024, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION]               = { "ev", -3, 3, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS]       = { "brightness", -3, 3, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_CONTRAST]         = { "contrast", -3, 3, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_SATURATION]       = { "saturation", -4, 4, 0, true, false },
    /* Not the table's: with the table's, reds come out grey. */
    [CAMERA_DRV_REGISTERS_CONTROL_VIVID]            = { "vivid", 0, 1, 1, false, false },
    [CAMERA_DRV_REGISTERS_CONTROL_EFFECT]           = { "effect", 0, (int32_t)CAMERA_DRV_REGISTERS_EFFECT_COUNT - 1, 0, true,
                                    false },
    [CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS]        = { "sharpness", -3, 3, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_DENOISE]          = { "denoise", 0, 8, 0, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_FLICKER]          = { "flicker", 0, (int32_t)CAMERA_DRV_REGISTERS_FLICKER_COUNT - 1,
                                    (int32_t)CAMERA_DRV_REGISTERS_FLICKER_AUTO, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING]     = { "gain_ceiling", 16, 1023, CEILING_TABLE, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION]  = { "lens_correction", 0, 1, 1, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION] = { "pixel_correction", 0, 1, 1, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAMMA]            = { "gamma", 0, 1, 1, true, false },
    [CAMERA_DRV_REGISTERS_CONTROL_NIGHT]            = { "night", 0, 1, 0, true, true },
};

const camera_drv_registers_control_info_t *camera_drv_registers_info(camera_drv_registers_control_t c)
{
    return ((unsigned)c < (unsigned)CAMERA_DRV_REGISTERS_CONTROL_COUNT) ? &INFO[c] : NULL;
}

void camera_drv_registers_defaults(int32_t *val)
{
    if (val == NULL) {
        return;
    }
    for (size_t c = 0U; c < (size_t)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        val[c] = INFO[c].dflt;
    }
}

/* ---- The baseline ------------------------------------------------------------
 *
 * The bytes the format table leaves in the registers the controls share or
 * restore. The table writes 0x5580 = 0x02 (saturation on, contrast off),
 * 0x5583 = 0x40, 0x5584 = 0x10, the colour matrix, the sharpening and
 * denoise thresholds and 0x3C01 = 0x34 (50/60 Hz found by the sensor); the
 * rest are the sensor's reset values, read all the same. */
enum {
    B_CMX4 = 0,             /* 0x5384..0x5389: the matrix's colour rows, 6 */
    B_SDE0 = 6,             /* 0x5580: the special digital effects' enables */
    B_SDE_U,                /* 0x5583: U saturation, or a fixed U           */
    B_SDE_V,                /* 0x5584: V saturation, or a fixed V           */
    B_SDE_YOFF,             /* 0x5585: the contrast's offset                */
    B_SDE_YGAIN,            /* 0x5586: the contrast's gain, 0x20 is 1x      */
    B_SDE_BRIGHT,           /* 0x5587: the brightness offset                */
    B_SDE_SIGN,             /* 0x5588: bit 3, the brightness's sign         */
    B_ISP3,                 /* 0x5003: bit 3 with an effect, as published   */
    B_CIP,                  /* 0x5308: bit 6 sharpening, bit 4 denoise      */
    B_SH_TH1,               /* 0x5300, 0x5301: sharpening thresholds        */
    B_SH_TH2,
    B_SH_OFF1,              /* 0x5302, 0x5303: sharpening offsets           */
    B_SH_OFF2,
    B_SHT_TH1,              /* 0x5309, 0x530A: the second threshold pair    */
    B_SHT_TH2,
    B_DNS,                  /* 0x5306: the denoise offset                   */
    B_FLK0,                 /* 0x3C00: bit 2, 50 Hz when manual             */
    B_FLK1,                 /* 0x3C01: bit 7, manual 50/60 Hz               */
    B_COUNT
};
_Static_assert(B_COUNT == (int)CAMERA_DRV_REGISTERS_BASELINE, "the baseline's size");

static const uint16_t BASELINE[CAMERA_DRV_REGISTERS_BASELINE] = {
    0x5384U, 0x5385U, 0x5386U, 0x5387U, 0x5388U, 0x5389U,
    0x5580U, 0x5583U, 0x5584U, 0x5585U, 0x5586U, 0x5587U, 0x5588U,
    0x5003U,
    0x5308U, 0x5300U, 0x5301U, 0x5302U, 0x5303U, 0x5309U, 0x530AU,
    0x5306U,
    0x3C00U, 0x3C01U,
};

/* ---- The 5 megapixel still ----------------------------------------------------------
 *
 * Where each value comes from (NOTICE.md says the same):
 *
 * [E] Espressif's esp_cam_sensor, Apache-2.0, which writes the same register at
 *     the same value: ov5640_mipi_2lane_24Minput_1280x720_rgb565_le_14fps.h reads
 *     the whole array (0..2623 x 0..1951), offsets 16 and 4, a frame of 2844 x
 *     1968, no subsampling, and the "anti-green" 0x3618, 0x3612, 0x3709, 0x370C;
 *     the DVP table for 800 x 600 has 0x4001 = 0x02.
 * [K] Kode's own values for the full array: 0x3708, 0x4004 and 0x4520,
 *     which Espressif's tables do not carry.
 * [D] derived here: the output size is the numbers 2592 and 1944 in the output
 *     registers' layout; the banding steps are the preview table's own
 *     convention (a whole mains period, in lines) for the longer line.
 * The PLL is the preview's, left alone: the pixel clock is the same 56 MHz.
 */

/* The auto exposure's banding steps are a whole mains period in lines of
 * 2 x HTS clocks: 20 ms at 50 Hz and 16.67 ms at 60 Hz. For the preview's HTS
 * of 1896 that is 295 (0x0127) and 246 (0x00F6), what its table writes. */
#define BAND_LINES(us) \
    ((uint16_t)(((uint32_t)(us) * (CAMERA_BSP_PCLK_HZ / 1000000U) + CAMERA_BSP_STILL_HTS) \
                / (2U * CAMERA_BSP_STILL_HTS)))
#define BAND50 BAND_LINES(20000U)
#define BAND60 BAND_LINES(16667U)

static const camera_drv_registers_write_t STILL[] = {
    /* The whole array, in the sensor's own addresses. [E] */
    { 0x3800U, 0xFFU, 0x00U }, { 0x3801U, 0xFFU, 0x00U },    /* x start 0        */
    { 0x3802U, 0xFFU, 0x00U }, { 0x3803U, 0xFFU, 0x00U },    /* y start 0        */
    { 0x3804U, 0xFFU, 0x0AU }, { 0x3805U, 0xFFU, 0x3FU },    /* x end 2623       */
    { 0x3806U, 0xFFU, 0x07U }, { 0x3807U, 0xFFU, 0x9FU },    /* y end 1951       */
    /* ... output 2592 x 1944. [D] */
    { 0x3808U, 0xFFU, 0x0AU }, { 0x3809U, 0xFFU, 0x20U },
    { 0x380AU, 0xFFU, 0x07U }, { 0x380BU, 0xFFU, 0x98U },
    /* ... in a frame of 2844 x 1968. [E] */
    { 0x380CU, 0xFFU, 0x0BU }, { 0x380DU, 0xFFU, 0x1CU },
    { 0x380EU, 0xFFU, 0x07U }, { 0x380FU, 0xFFU, 0xB0U },
    /* ... the ISP's window inside it, 16 across and 4 down. [E] */
    { 0x3810U, 0xFFU, 0x00U }, { 0x3811U, 0xFFU, 0x10U },
    { 0x3812U, 0xFFU, 0x00U }, { 0x3813U, 0xFFU, 0x04U },
    /* No subsampling. [E] */
    { 0x3814U, 0xFFU, 0x11U }, { 0x3815U, 0xFFU, 0x11U },
    { 0x4520U, 0xFFU, 0x10U },                                /* [K]              */
    /* The analogue side of the full mode. 0x3618, 0x3612, 0x3709, 0x370C [E];
     * 0x3708 [K] (the preview table's is 0x64). */
    { 0x3618U, 0xFFU, 0x04U }, { 0x3612U, 0xFFU, 0x2BU },
    { 0x3708U, 0xFFU, 0x63U }, { 0x3709U, 0xFFU, 0x12U }, { 0x370CU, 0xFFU, 0x00U },
    /* The black-level calibration: 0x4001 as the preview's table [E], 0x4004 at 6
     * lines for the full frame (the preview's has 2) [K]. */
    { 0x4001U, 0xFFU, 0x02U }, { 0x4004U, 0xFFU, 0x06U },
    /* The ISP does not scale: bit 5 of 0x5001, which the preview's table sets. */
    { 0x5001U, 0x20U, 0x00U },
    /* The banding steps for the longer line: a lamp on the mains must not band the
     * photo. Big-endian pairs. [D] */
    { 0x3A08U, 0xFFU, (uint8_t)(BAND50 >> 8) }, { 0x3A09U, 0xFFU, (uint8_t)(BAND50 & 0xFFU) },
    { 0x3A0AU, 0xFFU, (uint8_t)(BAND60 >> 8) }, { 0x3A0BU, 0xFFU, (uint8_t)(BAND60 & 0xFFU) },
};

const camera_drv_registers_write_t *camera_drv_registers_still(size_t *n)
{
    if (n != NULL) {
        *n = sizeof(STILL) / sizeof(STILL[0]);
    }
    return STILL;
}

void camera_drv_registers_still_orient(bool vflip, bool mirror, camera_drv_registers_write_t *out)
{
    /* 0x3820 bit 6 is the full mode's (OmniVision's 0x40); bits 2:1 the flips;
     * bit 0, the vertical binning, is off. 0x4514 is esp32-camera's table for
     * the unbinned sensor (Apache-2.0): 0x88 straight, 0xBB mirrored, 0x00 once
     * it is flipped. */
    out[0] = (camera_drv_registers_write_t){ CAMERA_DRV_REGISTERS_ADDRESS_TIMING_20, 0xFFU,
                                (uint8_t)(0x40U | (vflip ? CAMERA_DRV_REGISTERS_FLIP_BITS : 0U)) };
    out[1] = (camera_drv_registers_write_t){ CAMERA_DRV_REGISTERS_ADDRESS_TIMING_21, 0xFFU, (uint8_t)(mirror ? CAMERA_DRV_REGISTERS_FLIP_BITS : 0U) };
    out[2] = (camera_drv_registers_write_t){ 0x4514U, 0xFFU,
                                (uint8_t)(vflip ? 0x00U : (mirror ? 0xBBU : 0x88U)) };
}

void camera_drv_registers_still_for_module(bool rotated, camera_drv_registers_write_t *out)
{
    camera_drv_registers_still_orient((rotated ? CAMERA_BSP_ROTATED_VFLIP : CAMERA_BSP_UP_VFLIP) != 0U,
                          (rotated ? CAMERA_BSP_ROTATED_MIRROR : CAMERA_BSP_UP_MIRROR) != 0U, out);
}

int32_t camera_drv_registers_still_exposure(int32_t x16)
{
    if (x16 <= 0) {
        return 0;
    }
    int64_t v = ((int64_t)x16 * CAMERA_BSP_HTS + (CAMERA_BSP_STILL_HTS / 2U)) / CAMERA_BSP_STILL_HTS;
    int64_t const max = ((int64_t)CAMERA_BSP_STILL_VTS - 4) * 16;
    return (int32_t)((v > max) ? max : v);
}

void camera_drv_registers_vcm(uint16_t pos, camera_drv_registers_write_t *out)
{
    if (pos > 1023U) {
        pos = 1023U;
    }
    /* The high part first. The low register's
     * low nibble is not the DAC's and is left as it is. */
    out[0] = (camera_drv_registers_write_t){ CAMERA_DRV_REGISTERS_ADDRESS_VCM_HIGH, CAMERA_DRV_REGISTERS_VCM_HIGH_MASK, (uint8_t)(pos >> 4) };
    out[1] = (camera_drv_registers_write_t){ CAMERA_DRV_REGISTERS_ADDRESS_VCM_LOW, CAMERA_DRV_REGISTERS_VCM_LOW_MASK, (uint8_t)((pos & 0x0FU) << 4) };
}

uint16_t camera_drv_registers_vcm_position(uint8_t high, uint8_t low)
{
    return (uint16_t)((((uint16_t)(high & CAMERA_DRV_REGISTERS_VCM_HIGH_MASK)) << 4) | (uint16_t)(low >> 4));
}

const uint16_t *camera_drv_registers_baseline(void)
{
    return BASELINE;
}

/* ---- Plain fields ------------------------------------------------------------
 *
 * A field spread over registers, most significant first: bits of the value
 * from the top down, each register's share under its mask at bit 0. A flag
 * is one or more bits that are all set for 1 and all clear for 0. */
typedef struct {
    uint16_t reg[3];
    uint8_t  mask[3];   /* the bits of the register the value occupies     */
    uint8_t  count;     /* 0: not a plain field                            */
    bool     flag;
} field_t;

/* The exposure is 0x3500[3:0], 0x3501, 0x3502; the gain 0x350A[1:0], 0x350B;
 * each white balance gain xxH[3:0], xxL; the gain ceiling 0x3A18[1:0],
 * 0x3A19. 0x5000 holds the lens correction (bit 7), the gamma (bit 5) and
 * the black and white pixel corrections (bits 2 and 1), as esp32-camera
 * sets them; night mode is 0x3A00 bit 2. */
static const field_t FIELD[CAMERA_DRV_REGISTERS_CONTROL_COUNT] = {
    [CAMERA_DRV_REGISTERS_CONTROL_TEST_PATTERN]     = { { 0x503DU }, { 0x80U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_MANUAL]        = { { 0x3503U }, { 0x01U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN_MANUAL]        = { { 0x3503U }, { 0x02U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE]         = { { 0x3500U, 0x3501U, 0x3502U }, { 0x0FU, 0xFFU, 0xFFU }, 3U,
                                    false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN]             = { { 0x350AU, 0x350BU }, { 0x03U, 0xFFU }, 2U, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED]          = { { 0x3400U, 0x3401U }, { 0x0FU, 0xFFU }, 2U, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN]        = { { 0x3402U, 0x3403U }, { 0x0FU, 0xFFU }, 2U, false },
    [CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE]         = { { 0x3404U, 0x3405U }, { 0x0FU, 0xFFU }, 2U, false },
    [CAMERA_DRV_REGISTERS_CONTROL_GAIN_CEILING]     = { { 0x3A18U, 0x3A19U }, { 0x03U, 0xFFU }, 2U, false },
    [CAMERA_DRV_REGISTERS_CONTROL_LENS_CORRECTION]  = { { 0x5000U }, { 0x80U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_PIXEL_CORRECTION] = { { 0x5000U }, { 0x06U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_GAMMA]            = { { 0x5000U }, { 0x20U }, 1U, true },
    [CAMERA_DRV_REGISTERS_CONTROL_NIGHT]            = { { 0x3A00U }, { 0x04U }, 1U, true },
};

static uint8_t low_bit(uint8_t mask)
{
    uint8_t s = 0U;
    while ((s < 8U) && (((mask >> s) & 1U) == 0U)) {
        s++;
    }
    return s;
}

static uint8_t bit_count(uint8_t mask)
{
    uint8_t n = 0U;
    for (uint8_t i = 0U; i < 8U; i++) {
        n += (uint8_t)((mask >> i) & 1U);
    }
    return n;
}

static size_t field(const field_t *f, int32_t value, camera_drv_registers_write_t *out)
{
    if (f->flag) {
        out[0] = (camera_drv_registers_write_t){ f->reg[0], f->mask[0], (value != 0) ? f->mask[0] : 0U };
        return 1U;
    }
    uint32_t v = (uint32_t)value;
    /* From the least significant register up, taking each one's bits. */
    for (int i = (int)f->count - 1; i >= 0; i--) {
        uint8_t const mask  = f->mask[i];
        uint8_t const width = bit_count(mask);
        uint8_t const shift = low_bit(mask);
        uint8_t const part  = (uint8_t)(v & ((1U << width) - 1U));
        out[i] = (camera_drv_registers_write_t){ f->reg[i], mask, (uint8_t)((uint8_t)(part << shift) & mask) };
        v >>= width;
    }
    return f->count;
}

/* ---- The exposure target, in seven steps ------------------------------------
 *
 * The auto exposure holds the picture's mean between a high and a low mark
 * (0x3A0F/0x3A10, and 0x3A1B/0x3A1E to re-enter), and steps fast outside
 * 0x3A11/0x3A1F. Step 0 is exactly what the sensor's format table writes, so
 * EV 0 is the format table's picture. The other steps move
 * the centre by half a stop each (44 x 2^(n/2)), keep the format table's
 * spacing of eight between the marks and its fast zone at twice the high
 * and half the low. Derived here, not taken from the application note. */
#define EV_STEPS 7U
static const uint16_t EV_REG[6] = { 0x3A0FU, 0x3A10U, 0x3A1BU, 0x3A1EU, 0x3A11U, 0x3A1FU };
static const uint8_t EV_VAL[EV_STEPS][6] = {
    { 0x14U, 0x0CU, 0x14U, 0x0AU, 0x28U, 0x06U },   /* -3 */
    { 0x1AU, 0x12U, 0x1AU, 0x10U, 0x34U, 0x09U },   /* -2 */
    { 0x23U, 0x1BU, 0x23U, 0x19U, 0x46U, 0x0DU },   /* -1 */
    { 0x30U, 0x28U, 0x30U, 0x26U, 0x60U, 0x14U },   /*  0: the format table's */
    { 0x42U, 0x3AU, 0x42U, 0x38U, 0x84U, 0x1DU },   /* +1 */
    { 0x5CU, 0x54U, 0x5CU, 0x52U, 0xB8U, 0x2AU },   /* +2 */
    { 0x80U, 0x78U, 0x80U, 0x76U, 0xFFU, 0x3CU },   /* +3 */
};

static size_t ev(int32_t value, camera_drv_registers_write_t *out)
{
    const uint8_t *row = EV_VAL[(size_t)(value + 3)];
    for (size_t i = 0U; i < 6U; i++) {
        out[i] = (camera_drv_registers_write_t){ EV_REG[i], 0xFFU, row[i] };
    }
    return 6U;
}

/* ---- The white balance -------------------------------------------------------
 *
 * 0x3406 bit 0 makes the gains manual - first, or the automatic balance
 * writes over them on the next frame - and then 0x3400..0x3405 hold red,
 * green and blue, 12 bits each, 0x400 = 1x. The presets are esp32-camera's
 * (set_wb_mode). The neutral balance is the owner's loop, which starts from
 * the sensor's automatic one: here it is the same as auto. */
static const uint16_t PRESET[4][3] = {
    { 0x5E0U, 0x410U, 0x540U },   /* sunny  */
    { 0x650U, 0x410U, 0x4F0U },   /* cloudy */
    { 0x520U, 0x410U, 0x660U },   /* office */
    { 0x420U, 0x3F0U, 0x710U },   /* home   */
};

static size_t gains(uint16_t r, uint16_t g, uint16_t b, camera_drv_registers_write_t *out)
{
    size_t n = 0U;
    n += field(&FIELD[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED], (int32_t)r, &out[n]);
    n += field(&FIELD[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN], (int32_t)g, &out[n]);
    n += field(&FIELD[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE], (int32_t)b, &out[n]);
    return n;
}

static size_t wb(const int32_t *v, camera_drv_registers_write_t *out)
{
    camera_drv_registers_white_balance_t const mode = (camera_drv_registers_white_balance_t)v[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE];
    bool const manual = (mode != CAMERA_DRV_REGISTERS_WHITE_BALANCE_AUTO) && (mode != CAMERA_DRV_REGISTERS_WHITE_BALANCE_NEUTRAL);

    out[0] = (camera_drv_registers_write_t){ 0x3406U, 0x01U, manual ? 0x01U : 0x00U };
    if (!manual) {
        return 1U;
    }
    if (mode == CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL) {
        return 1U + gains((uint16_t)v[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED], (uint16_t)v[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN],
                          (uint16_t)v[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE], &out[1]);
    }
    const uint16_t *p = PRESET[(size_t)mode - (size_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_SUNNY];
    return 1U + gains(p[0], p[1], p[2], &out[1]);
}

/* ---- The colour matrix: saturation ------------------------------------------
 *
 * The matrix's first row (0x5381..0x5383) makes the luma; the other six
 * (0x5384..0x5389) the colour. esp32-camera's saturation levels keep the
 * first row and scale the other six by about a tenth a step; here the same
 * tenth is applied to the format table's own matrix, so level 0 is the
 * table's exactly. The signs, 0x538A/0x538B, are not touched. */
static size_t saturation(const camera_drv_registers_context_t *x, camera_drv_registers_write_t *out)
{
    int32_t const k = 10 + x->val[CAMERA_DRV_REGISTERS_CONTROL_SATURATION];

    for (size_t i = 0U; i < 6U; i++) {
        uint32_t m = (((uint32_t)x->base[B_CMX4 + i] * (uint32_t)k) + 5U) / 10U;
        if (m > 0xFFU) {
            m = 0xFFU;
        }
        out[i] = (camera_drv_registers_write_t){ BASELINE[B_CMX4 + i], 0xFFU, (uint8_t)m };
    }
    return 6U;
}

/* ---- The special digital effects ---------------------------------------------
 *
 * 0x5580 enables them: bit 6 the negative, bits 4 and 3 a fixed V and U
 * (0x5584/0x5583 then hold the colour instead of the saturation), bit 2 the
 * contrast and brightness, bit 1 the saturation. The contrast is a gain
 * (0x5586) and an offset (0x5585), the brightness an offset (0x5587) with
 * its sign in 0x5588 bit 3. All of them are written together, from every
 * control that shares them:
 *  - with no effect, no contrast and no brightness, the table's bytes;
 *  - VIVID sets V's saturation to U's (the table's V is 0x10 against U's
 *    0x40: a red field comes out nearly grey);
 *  - the contrast steps are Adafruit's (CircuitPython_OV5640) and the
 *    brightness steps esp32-camera's; the effects' enables and fixed
 *    colours are the table both publish, with 0x5003 bit 3 as they set it.
 *    The negative keeps the saturation the picture would have had. */
static const uint8_t CONTRAST[7][2] = {        /* 0x5586, 0x5585 */
    { 0x14U, 0x14U }, { 0x18U, 0x18U }, { 0x1CU, 0x1CU },
    { 0x20U, 0x00U },
    { 0x24U, 0x10U }, { 0x28U, 0x18U }, { 0x2CU, 0x1CU },
};

typedef struct {
    uint8_t ctrl;   /* bits for 0x5580                       */
    uint8_t u;      /* fixed U and V; 0 for the picture's own */
    uint8_t v;
} effect_t;

static const effect_t EFFECT[CAMERA_DRV_REGISTERS_EFFECT_COUNT] = {
    [CAMERA_DRV_REGISTERS_EFFECT_NONE]      = { 0x00U, 0x00U, 0x00U },
    [CAMERA_DRV_REGISTERS_EFFECT_NEGATIVE]  = { 0x46U, 0x00U, 0x00U },
    [CAMERA_DRV_REGISTERS_EFFECT_GRAYSCALE] = { 0x1EU, 0x80U, 0x80U },
    [CAMERA_DRV_REGISTERS_EFFECT_RED]       = { 0x1EU, 0x80U, 0xC0U },
    [CAMERA_DRV_REGISTERS_EFFECT_GREEN]     = { 0x1EU, 0x60U, 0x60U },
    [CAMERA_DRV_REGISTERS_EFFECT_BLUE]      = { 0x1EU, 0xA0U, 0x40U },
    [CAMERA_DRV_REGISTERS_EFFECT_SEPIA]     = { 0x1EU, 0x40U, 0xA0U },
};

#define SDE_NEGATIVE 0x40U
#define SDE_FIXED_UV 0x18U
#define SDE_Y        0x04U
#define SDE_ISP3_BIT 0x08U

static size_t sde(const camera_drv_registers_context_t *x, camera_drv_registers_write_t *out)
{
    const int32_t *v = x->val;
    const uint8_t *b = x->base;
    const effect_t *e = &EFFECT[(size_t)v[CAMERA_DRV_REGISTERS_CONTROL_EFFECT]];
    int32_t const contrast = v[CAMERA_DRV_REGISTERS_CONTROL_CONTRAST];
    int32_t const bright   = v[CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS];

    uint8_t ctrl  = (uint8_t)(b[B_SDE0] & (uint8_t)~(SDE_NEGATIVE | SDE_FIXED_UV));
    uint8_t u     = b[B_SDE_U];
    uint8_t vv    = (v[CAMERA_DRV_REGISTERS_CONTROL_VIVID] != 0) ? b[B_SDE_U] : b[B_SDE_V];
    uint8_t yoff  = b[B_SDE_YOFF];
    uint8_t ygain = b[B_SDE_YGAIN];
    uint8_t ybri  = b[B_SDE_BRIGHT];
    uint8_t sign  = (uint8_t)(b[B_SDE_SIGN] & 0x08U);
    uint8_t isp3  = b[B_ISP3];

    if (e->ctrl != 0U) {
        ctrl |= e->ctrl;
        isp3 |= SDE_ISP3_BIT;
        if ((e->ctrl & SDE_FIXED_UV) != 0U) {
            u  = e->u;
            vv = e->v;
        }
    }
    if ((e->ctrl != 0U) || (contrast != 0) || (bright != 0)) {
        ctrl |= SDE_Y;
        ygain = CONTRAST[(size_t)(contrast + 3)][0];
        yoff  = CONTRAST[(size_t)(contrast + 3)][1];
        ybri  = (uint8_t)((uint32_t)((bright < 0) ? -bright : bright) << 4);
        sign  = (bright < 0) ? 0x08U : 0x00U;
    }
    out[0] = (camera_drv_registers_write_t){ 0x5580U, 0xFFU, ctrl };
    out[1] = (camera_drv_registers_write_t){ 0x5583U, 0xFFU, u };
    out[2] = (camera_drv_registers_write_t){ 0x5584U, 0xFFU, vv };
    out[3] = (camera_drv_registers_write_t){ 0x5586U, 0xFFU, ygain };
    out[4] = (camera_drv_registers_write_t){ 0x5585U, 0xFFU, yoff };
    out[5] = (camera_drv_registers_write_t){ 0x5587U, 0xFFU, ybri };
    out[6] = (camera_drv_registers_write_t){ 0x5588U, 0x08U, sign };
    out[7] = (camera_drv_registers_write_t){ 0x5003U, 0xFFU, isp3 };
    return 8U;
}

/* ---- Sharpening and denoise ----------------------------------------------------
 *
 * 0x5308 bit 6 and bit 4 pick the sharpening's and the denoise's own
 * settings or ours. Level 0 of each is the table's: its bits and its
 * thresholds and offsets put back. The other sharpening levels are
 * esp32-camera's (set_sharpness): thresholds of 0x10 and offsets of
 * (level + 3) x 8; the denoise levels too (set_denoise): 0x5308 bit 4 and
 * an offset of (level - 1) x 4. */
static size_t sharpness(const camera_drv_registers_context_t *x, camera_drv_registers_write_t *out)
{
    const uint8_t *b = x->base;
    int32_t const l = x->val[CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS];

    if (l == 0) {
        out[0] = (camera_drv_registers_write_t){ 0x5308U, 0x40U, (uint8_t)(b[B_CIP] & 0x40U) };
        for (size_t i = 0U; i < 6U; i++) {
            out[1U + i] = (camera_drv_registers_write_t){ BASELINE[B_SH_TH1 + i], 0xFFU, b[B_SH_TH1 + i] };
        }
        return 7U;
    }
    uint8_t const off2 = (uint8_t)((l + 3) * 8);
    out[0] = (camera_drv_registers_write_t){ 0x5308U, 0x40U, 0x00U };
    out[1] = (camera_drv_registers_write_t){ 0x5300U, 0xFFU, 0x10U };
    out[2] = (camera_drv_registers_write_t){ 0x5301U, 0xFFU, 0x10U };
    out[3] = (camera_drv_registers_write_t){ 0x5302U, 0xFFU, (uint8_t)(off2 + 1U) };
    out[4] = (camera_drv_registers_write_t){ 0x5303U, 0xFFU, off2 };
    out[5] = (camera_drv_registers_write_t){ 0x5309U, 0xFFU, 0x10U };
    out[6] = (camera_drv_registers_write_t){ 0x530AU, 0xFFU, 0x10U };
    return 7U;
}

static size_t denoise(const camera_drv_registers_context_t *x, camera_drv_registers_write_t *out)
{
    const uint8_t *b = x->base;
    int32_t const l = x->val[CAMERA_DRV_REGISTERS_CONTROL_DENOISE];

    if (l == 0) {
        out[0] = (camera_drv_registers_write_t){ 0x5308U, 0x10U, (uint8_t)(b[B_CIP] & 0x10U) };
        out[1] = (camera_drv_registers_write_t){ 0x5306U, 0xFFU, b[B_DNS] };
    } else {
        out[0] = (camera_drv_registers_write_t){ 0x5308U, 0x10U, 0x10U };
        out[1] = (camera_drv_registers_write_t){ 0x5306U, 0xFFU, (uint8_t)((l - 1) * 4) };
    }
    return 2U;
}

/* ---- The mains frequency -------------------------------------------------------
 *
 * 0x3C01 bit 7 makes the choice manual and 0x3C00 bit 2 picks 50 Hz. Under
 * some LED lights the sensor's own detection makes a continuous focus hunt,
 * which a fixed frequency avoids; the SDK keeps the table's detection unless
 * the app picks one. Exposure bands, not the frame, move: nothing here
 * changes the clocks or the frame timing. */
static size_t flicker(const camera_drv_registers_context_t *x, camera_drv_registers_write_t *out)
{
    const uint8_t *b = x->base;
    camera_drv_registers_flicker_t const f = (camera_drv_registers_flicker_t)x->val[CAMERA_DRV_REGISTERS_CONTROL_FLICKER];

    if (f == CAMERA_DRV_REGISTERS_FLICKER_AUTO) {
        out[0] = (camera_drv_registers_write_t){ 0x3C01U, 0x80U, (uint8_t)(b[B_FLK1] & 0x80U) };
        out[1] = (camera_drv_registers_write_t){ 0x3C00U, 0x04U, (uint8_t)(b[B_FLK0] & 0x04U) };
    } else {
        out[0] = (camera_drv_registers_write_t){ 0x3C01U, 0x80U, 0x80U };
        out[1] = (camera_drv_registers_write_t){ 0x3C00U, 0x04U, (f == CAMERA_DRV_REGISTERS_FLICKER_50HZ) ? 0x04U : 0x00U };
    }
    return 2U;
}

/* ---- Encoding ------------------------------------------------------------------ */

static bool in_range(const camera_drv_registers_context_t *x)
{
    for (size_t c = 0U; c < (size_t)CAMERA_DRV_REGISTERS_CONTROL_COUNT; c++) {
        if ((x->val[c] < INFO[c].min) || (x->val[c] > INFO[c].max)) {
            return false;
        }
    }
    return true;
}

bool camera_drv_registers_encode(camera_drv_registers_control_t c, const camera_drv_registers_context_t *ctx, camera_drv_registers_write_t *out, size_t *n)
{
    if ((camera_drv_registers_info(c) == NULL) || (ctx == NULL) || (out == NULL) || (n == NULL)
        || !in_range(ctx)) {
        return false;
    }
    const int32_t *v = ctx->val;
    size_t k = 0U;

    switch (c) {
    case CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE_COMPENSATION:
        k = ev(v[c], out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE:
        k = wb(v, out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED:
    case CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN:
    case CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE:
        /* Only the manual balance holds them; the others write over them. */
        k = (v[CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE] == (int32_t)CAMERA_DRV_REGISTERS_WHITE_BALANCE_MANUAL) ? field(&FIELD[c], v[c], out) : 0U;
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_SATURATION:
        k = saturation(ctx, out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_BRIGHTNESS:
    case CAMERA_DRV_REGISTERS_CONTROL_CONTRAST:
    case CAMERA_DRV_REGISTERS_CONTROL_VIVID:
    case CAMERA_DRV_REGISTERS_CONTROL_EFFECT:
        k = sde(ctx, out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_SHARPNESS:
        k = sharpness(ctx, out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_DENOISE:
        k = denoise(ctx, out);
        break;
    case CAMERA_DRV_REGISTERS_CONTROL_FLICKER:
        k = flicker(ctx, out);
        break;
    default:
        k = field(&FIELD[c], v[c], out);
        break;
    }
    *n = k;
    return true;
}

/* ---- Reading back ----------------------------------------------------------------
 *
 * Only what the sensor's automatics move is read: the exposure and the gain
 * while the exposure is automatic, the white-balance gains while the
 * balance is. Every other control is what was last set. */
static bool live(camera_drv_registers_control_t c)
{
    return (c == CAMERA_DRV_REGISTERS_CONTROL_EXPOSURE) || (c == CAMERA_DRV_REGISTERS_CONTROL_GAIN) || (c == CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_RED)
           || (c == CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_GREEN) || (c == CAMERA_DRV_REGISTERS_CONTROL_WHITE_BALANCE_BLUE);
}

bool camera_drv_registers_reads(camera_drv_registers_control_t c, uint16_t *out, size_t *n)
{
    if ((camera_drv_registers_info(c) == NULL) || (out == NULL) || (n == NULL)) {
        return false;
    }
    *n = 0U;
    if (!live(c)) {
        return true;
    }
    const field_t *f = &FIELD[c];
    for (size_t i = 0U; i < f->count; i++) {
        out[i] = f->reg[i];
    }
    *n = f->count;
    return true;
}

bool camera_drv_registers_decode(camera_drv_registers_control_t c, const uint8_t *bytes, size_t n, int32_t *value)
{
    if ((camera_drv_registers_info(c) == NULL) || (bytes == NULL) || (value == NULL) || !live(c)) {
        return false;
    }
    const field_t *f = &FIELD[c];
    if (n != f->count) {
        return false;
    }
    uint32_t v = 0U;
    for (size_t i = 0U; i < f->count; i++) {
        uint8_t const mask = f->mask[i];
        v = (v << bit_count(mask)) | (uint32_t)((uint8_t)(bytes[i] & mask) >> low_bit(mask));
    }
    *value = (int32_t)v;
    return true;
}

/* ---- The quiet sequence ------------------------------------------------------
 *
 * The stream off first (frames masked), then the pads to inputs, then the
 * software power-down with the rest of SYS_CTRL0 at its reset value (0x02).
 * Nothing here resets the sensor: a software reset outside the format table
 * spoils the colour. */
static const camera_drv_registers_write_t QUIET[] = {
    { CAMERA_DRV_REGISTERS_ADDRESS_FRAME_CONTROL, 0xFFU, 0x0FU },
    { CAMERA_DRV_REGISTERS_ADDRESS_PAD_OE1,    0xFFU, 0x00U },
    { CAMERA_DRV_REGISTERS_ADDRESS_PAD_OE2,    0xFFU, 0x00U },
    { CAMERA_DRV_REGISTERS_ADDRESS_SYS_CTRL0,  0xFFU, 0x42U },
};

const camera_drv_registers_write_t *camera_drv_registers_quiet(size_t *n)
{
    if (n != NULL) {
        *n = sizeof(QUIET) / sizeof(QUIET[0]);
    }
    return QUIET;
}
