/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_pixels.h
 * @brief The sensor's pixels: their bytes read right, a frame measured, and a
 *        frame fitted into a picture of any size, upright.
 *
 * Plain C - no ESP-IDF, no FreeRTOS - so tests/host runs it as it is.
 *
 * THE BYTE ORDER. The format is the driver's "RGB565_BE", which
 * sets FORMAT_CTRL00 = 0x6F: each pixel arrives as two bytes, G3B5 first and
 * R5G3 second, and the DVP writes them to memory in that order. Read as a
 * uint16_t on the little-endian P4 that is plain RGB565, red in bits 15..11,
 * as the sensor's colour bars confirm. These functions take the bytes as they
 * are, so nothing here depends on the host's endianness. The hardware JPEG
 * encoder takes the same bytes (pixel_reverse false), and so does the Dot's
 * glass through LVGL's RGB565.
 */
#ifndef CAMERA_DRV_PIXELS_H_
#define CAMERA_DRV_PIXELS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Under this on every channel, at 8 bits, a pixel is black. */
#define CAMERA_DRV_PIXELS_BLACK_BELOW  8U

/** @brief One pixel's channels at 8 bits, from its two bytes as sent. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} camera_drv_pixels_rgb_t;

/** @brief One pixel, read from the two bytes at @p px. */
camera_drv_pixels_rgb_t camera_drv_pixels_rgb(const uint8_t *px);

/** @brief One pixel's luma, 0..255 (BT.601, integer). */
uint8_t camera_drv_pixels_luma(const uint8_t *px);

/** @brief The two bytes, as sent, of an 8-bit colour. */
void camera_drv_pixels_put(uint8_t *px, uint8_t r, uint8_t g, uint8_t b);

/** @brief A frame, sampled. */
typedef struct {
    uint8_t  max;     /**< The brightest channel of any sample, 0..255.       */
    uint8_t  luma;    /**< The mean luma of the samples, 0..255.              */
    uint32_t samples; /**< How many pixels were read.                        */
} camera_drv_pixels_statistics_t;

/**
 * @brief Sample one pixel in @p step of a frame of @p n pixels.
 * @param frame The frame's bytes, two a pixel, as sent.
 * @param n     Pixels in the frame.
 * @param step  1 reads every pixel; the owner reads one in 64.
 */
camera_drv_pixels_statistics_t camera_drv_pixels_statistics(const uint8_t *frame, uint32_t n, uint32_t step);

/** @brief Whether a frame so sampled is black: no channel above the floor. */
bool camera_drv_pixels_black(const camera_drv_pixels_statistics_t *s);

/**
 * @brief How sharp a frame is: a thousand times the mean step of luma
 *        between neighbours, across and down, over the centre half of each
 *        side (a quarter of the area), one pair every two columns and four
 *        rows - about 30000 steps in an 800 x 600 frame and a millisecond on the P4.
 *
 * Relative: it compares one framing before and after focusing, never two
 * scenes. The autofocus's policy (camera_drv_autofocus.h) lives on it.
 *
 * @return 0 for a frame too small to measure.
 */
uint32_t camera_drv_pixels_sharpness(const uint8_t *frame, uint16_t w, uint16_t h);

/** @brief How a frame fills a picture of another shape. */
typedef enum {
    CAMERA_DRV_PIXELS_FIT_COVER   = 0, /**< Fill it, cropping the frame's centre: no bands. */
    CAMERA_DRV_PIXELS_FIT_CONTAIN = 1, /**< All of the frame, black bands round it.         */
    CAMERA_DRV_PIXELS_FIT_STRETCH = 2, /**< All of the frame, to the picture's shape.       */
} camera_drv_pixels_fit_mode_t;

/** @brief Everything a fit needs besides the two buffers. */
typedef struct {
    uint16_t        src_w;   /**< The frame.                                  */
    uint16_t        src_h;
    uint16_t        dst_w;   /**< The picture.                                */
    uint16_t        dst_h;
    camera_drv_pixels_fit_mode_t mode;
    bool            turn;    /**< Turn the frame by 180 degrees.              */
    bool            mirror;  /**< Then flip it left to right.                 */
    bool            swap;    /**< Write each pixel's bytes the other way round:
                                  high byte first, a big-endian RGB565.       */
    uint16_t        zoom_x16;/**< A digital zoom: 16 the whole frame, 32 its
                                  centre half, up to 256 (16x); 0 counts as
                                  16. It crops; it adds no detail.             */
    int16_t         pan_x;   /**< Where the zoom looks, from the centre, in
                                  thousandths of the upright picture: -500
                                  the left edge, 500 the right. Kept inside
                                  the frame.                                  */
    int16_t         pan_y;   /**< The same, -500 the top.                     */
    bool            smooth;  /**< Where the picture is smaller than the window
                                  of the frame it shows, average the 2 x 2
                                  block under each sample instead of taking one
                                  pixel of it: no jagged edges in a reduction.
                                  Four reads a pixel; nothing on an axis that
                                  is not reduced. Off is the nearest pixel.   */
} camera_drv_pixels_fit_t;

/** @brief The closest digital zoom: 16x. */
#define CAMERA_DRV_PIXELS_ZOOM_MAX_X16     256U

/**
 * @brief Fit a frame into a picture: the nearest pixel, or with
 *        camera_drv_pixels_fit_t::smooth the mean of the 2 x 2 block there.
 *
 * Every pixel of @p dst is written. A picture bigger than the frame is filled
 * by repeating pixels: this is for previews, thumbnails and models, not for
 * enlarging. @p src and @p dst must not overlap.
 *
 * @return false, @p dst untouched, for a NULL pointer or a zero size.
 */
bool camera_drv_pixels_fit(const uint8_t *src, uint8_t *dst, const camera_drv_pixels_fit_t *fit);

/**
 * @brief Where a pixel of a fitted picture comes from: the point of the
 *        upright frame that camera_drv_pixels_fit() draws at (@p x, @p y) of the
 *        picture. A touch on the screen becomes the point the autofocus
 *        should look at.
 *
 * Upright is how the app sees the frame, whatever way the module is turned,
 * so camera_drv_pixels_fit_t::turn, ::swap and ::smooth do not matter here; the mirror,
 * the zoom, the pan and the fill do.
 *
 * @param[out] fx,fy The frame's column and row, 0..src_w-1 and 0..src_h-1.
 * @return false, nothing out, for a NULL pointer, a zero size, a point outside
 *         the picture or in a black band (CAMERA_DRV_PIXELS_FIT_CONTAIN).
 */
bool camera_drv_pixels_point(const camera_drv_pixels_fit_t *fit, uint16_t x, uint16_t y, uint16_t *fx, uint16_t *fy);

/** @brief The largest scale a hardware fit takes, in sixteenths (the P4's PPA: 255 and 15/16). */
#define CAMERA_DRV_PIXELS_HW_Q16_MAX       4095U
/** @brief The most columns or rows a hardware fit may leave to be copied from its last one. */
#define CAMERA_DRV_PIXELS_HW_PAD_MAX       2U

/**
 * @brief A fit as a scaler that works in sixteenths makes it (the P4's PPA,
 *        camera_drv_ppa.h): one block of the frame, as the frame lies, scaled
 *        by @c q16 / 16 on each axis into the picture's top-left corner and
 *        flipped as asked; the last @c pad_x columns and @c pad_y rows it
 *        cannot reach are copies of the one before them.
 */
typedef struct {
    uint32_t in_x;    /**< The block, in the frame's own coordinates. */
    uint32_t in_y;
    uint32_t in_w;
    uint32_t in_h;
    uint32_t q16_x;   /**< The scale, in sixteenths.                  */
    uint32_t q16_y;
    uint32_t out_w;   /**< What the scaler writes: floor(in * q16 / 16). */
    uint32_t out_h;
    uint32_t pad_x;   /**< dst_w - out_w.                             */
    uint32_t pad_y;   /**< dst_h - out_h.                             */
    bool     flip_x;  /**< Reverse the columns.                       */
    bool     flip_y;  /**< Reverse the rows.                          */
} camera_drv_pixels_hw_t;

/**
 * @brief Plan the fit of @p fit for a scaler in sixteenths.
 *
 * The window is camera_drv_pixels_fit()'s, the zoom and the pan included. A
 * scale that is not a whole number of sixteenths is rounded down, when the
 * whole window then leaves no more than CAMERA_DRV_PIXELS_HW_PAD_MAX lines
 * to pad, or up, with the window narrowed round its centre to match: the
 * picture shows up to one sixteenth less of the frame than the software fit
 * (at 800 x 600 to 640 x 480: 788 x 591 of the 800 x 600). Cover and stretch only, and no
 * byte swap: a contain leaves bands, and the scaler would mix the swapped
 * bytes' colours.
 *
 * @return false, @p hw untouched, when the scaler cannot make this picture:
 *         a NULL pointer, a zero size, a contain, a swap, a scale outside
 *         1..CAMERA_DRV_PIXELS_HW_Q16_MAX sixteenths, a rounding that would
 *         narrow the window by more than a sixteenth (small scales: 0.4 is
 *         7/16, 9 % less), or more than CAMERA_DRV_PIXELS_HW_PAD_MAX
 *         columns or rows left over.
 */
bool camera_drv_pixels_hw_plan(const camera_drv_pixels_fit_t *fit, camera_drv_pixels_hw_t *hw);

/**
 * @brief Fill the columns and rows a hardware fit left over (@c pad_x,
 *        @c pad_y) with copies of the last one it wrote.
 *
 * @param dst  The picture, dst_w x dst_h RGB565.
 * @param fit  The fit.
 * @param hw   Its plan.
 */
void camera_drv_pixels_hw_pad(uint8_t *dst, const camera_drv_pixels_fit_t *fit, const camera_drv_pixels_hw_t *hw);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_PIXELS_H_ */
