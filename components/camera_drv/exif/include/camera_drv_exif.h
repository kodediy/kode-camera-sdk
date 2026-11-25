/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file camera_drv_exif.h
 * @brief The EXIF a photo carries: its date, its size, its exposure and,
 *        when the Dot has a fix, where it was taken.
 *
 * Plain C - no ESP-IDF, no FreeRTOS, no malloc - so tests/host runs it as it
 * is. The caller owns every buffer.
 *
 * WHY. The P4's hardware JPEG encoder writes the picture and nothing else: no
 * EXIF. A phone or a PC that opens the file then has no date, no size line
 * and no place to show on a map. This builds the one segment that gives
 * them, and says where in the JPEG it goes. It does not touch the JPEG.
 *
 * THE SEGMENT. camera_drv_exif_build() writes a complete APP1 segment: the marker
 * 0xFF 0xE1, a big-endian length (which counts itself, not the marker), the
 * identifier "Exif\0\0" and then a TIFF structure, little-endian ("II*\0"),
 * with up to three directories:
 *
 *   - IFD0:     Make, Model, Orientation, XResolution, YResolution,
 *               ResolutionUnit, Software, DateTime, a pointer to the Exif
 *               IFD and, when there is a position, one to the GPS IFD;
 *   - Exif IFD: ExifVersion, DateTimeOriginal, DateTimeDigitized,
 *               OffsetTime, OffsetTimeOriginal, OffsetTimeDigitized,
 *               ExposureTime, ISO, ColorSpace, PixelXDimension and
 *               PixelYDimension;
 *   - GPS IFD:  version, latitude and longitude with their references,
 *               altitude, and the UTC time and date of the fix.
 *
 * Tags are sorted ascending inside each directory, the next-IFD offset is 0
 * (no thumbnail), a value that does not fit the entry's four bytes is put
 * after the directories at an even offset, and every offset counts from the
 * first byte of the TIFF header, as the standard says.
 *
 * NO FLOATING POINT. A position comes in as degrees times 1e7 and leaves as
 * three rationals, degrees, minutes and seconds. The seconds are exact:
 * their denominator is 1000000, so 40.4168000 degrees north is 40/1 25/1
 * 480000/1000000. An exposure comes in as microseconds and leaves as the
 * exact fraction of a second, reduced: 10000 us is 1/100, 2000000 us is 2/1.
 *
 * THE TIME ZONE. The dates are the Dot's clock as it reads them, and the
 * three OffsetTime tags say how far that is from UTC ("+01:00"), so a
 * reader shows the hour the photo was taken wherever it is opened. Without
 * them a reader takes the dates for its own local time: a Dot whose clock is
 * in UTC would show an hour off in Madrid in winter. The GPS time is UTC.
 *
 * WHAT IS NOT WRITTEN. A thumbnail, a maker note and anything about the lens.
 * A field the caller does not know is left at 0 (or NULL, or false) and its
 * tag is left out - never a made-up value.
 *
 * Strings are ASCII, at most CAMERA_DRV_EXIF_STRING_MAX characters; a longer one is cut
 * and a byte outside 0x20..0x7E becomes '?', so the file never carries
 * something a reader would take for another encoding.
 */
#ifndef CAMERA_DRV_EXIF_H_
#define CAMERA_DRV_EXIF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest Make, Model or Software kept, not counting the NUL. */
#define CAMERA_DRV_EXIF_STRING_MAX   63U

/**
 * @brief The most camera_drv_exif_build() ever writes, with every field present and
 *        every string at its longest (a test holds it to this). A segment
 *        can never reach 64 KB.
 */
#define CAMERA_DRV_EXIF_MAX_LENGTH   832U

/** @brief A calendar date and time. Which clock is said where it is used. */
typedef struct {
    uint16_t year;   /**< 1..9999; 0 = unknown: its tags are left out.       */
    uint8_t  month;  /**< 1..12                                              */
    uint8_t  day;    /**< 1..31, and no more than the month has (leap years
                          counted)                                           */
    uint8_t  hour;   /**< 0..23                                              */
    uint8_t  minute; /**< 0..59                                              */
    uint8_t  second; /**< 0..59                                              */
} camera_drv_exif_time_t;

/** @brief Where the Dot was when the photo was taken. */
typedef struct {
    bool    valid;     /**< false: no GPS IFD and no pointer to one; the other
                            fields are then not looked at.                   */
    int32_t lat_e7;    /**< Degrees x 1e7, south negative: -900000000..900000000. */
    int32_t lon_e7;    /**< Degrees x 1e7, west negative: -1800000000..1800000000. */
    int32_t alt_mm;    /**< Millimetres above the sea, negative below it.     */
    bool    alt_valid; /**< false: no altitude tags.                          */
    camera_drv_exif_time_t utc; /**< The fix's time in UTC. GPSTimeStamp and
                              GPSDateStamp are written when year != 0.       */
} camera_drv_exif_gps_t;

/** @brief Everything a photo's EXIF is made of. Zero it first. */
typedef struct {
    uint16_t width;       /**< PixelXDimension; with height, both or neither. */
    uint16_t height;      /**< PixelYDimension; 0 = omit both.                */
    uint8_t  orientation; /**< EXIF 1..8; 1 is upright. A zeroed 0 is NOT
                               valid: set it.                               */
    camera_drv_exif_time_t taken;/**< The Dot's clock: DateTime, DateTimeOriginal and
                               DateTimeDigitized; year 0 = no date tags.     */
    bool     has_offset;  /**< true: the OffsetTime tags, with @c taken.      */
    int16_t  offset_min;  /**< How far @c taken is ahead of UTC, in minutes:
                               -720..840 (60 is Madrid in winter, 0 is UTC). */
    uint32_t exposure_us; /**< ExposureTime as an exact rational; 0 = omit.   */
    uint16_t iso;         /**< ISOSpeedRatings; 0 = omit.                     */
    const char *make;     /**< NULL = "Kode".                                 */
    const char *model;    /**< NULL = "Kode Dot Camera".                      */
    const char *software; /**< NULL = omit.                                   */
    camera_drv_exif_gps_t gps;
} camera_drv_exif_t;

/**
 * @brief Build the APP1 segment for @p e into @p out.
 *
 * @return The segment's length in bytes, marker included, or 0 - and
 *         @p out untouched - when @p cap is smaller than the segment, a
 *         pointer is NULL, the orientation is outside 1..8, a date or a time
 *         does not exist (month 13, 31 April, 29 February of a common year,
 *         hour 24, a year past 9999), an offset is past -12:00..+14:00 or
 *         a position is off the globe.
 *         A date that is not asked for (year 0, or gps.valid false) is not
 *         looked at.
 */
size_t camera_drv_exif_build(const camera_drv_exif_t *e, uint8_t *out, size_t cap);

/**
 * @brief How many bytes camera_drv_exif_build() writes for @p e: exactly its
 *        length, so a buffer of this size is the least that works. Never
 *        more than CAMERA_DRV_EXIF_MAX_LENGTH. 0 for an input it would refuse.
 */
size_t camera_drv_exif_size(const camera_drv_exif_t *e);

/**
 * @brief Where the APP1 goes in a JPEG of @p len bytes: right after the SOI.
 *
 * The caller writes jpg[0..k), the segment, then jpg[k..len).
 *
 * @return 2; or 0 when the buffer is not a JPEG (it must start 0xFF 0xD8 and
 *         have a marker after it) or already has an Exif APP1 among the
 *         segments before its image data - a second one would leave a reader
 *         guessing which to believe.
 */
size_t camera_drv_exif_insert_at(const uint8_t *jpg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_DRV_EXIF_H_ */
