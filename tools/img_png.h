/* img_png.h -- minimal PNG writer for validation captures.
 *
 * VALIDATION INSTRUMENT, NOT PART OF THE APPLICATION.
 *
 * The project may not add dependencies casually, and it certainly should not add
 * zlib just to save a debug screenshot. PNG's container permits DEFLATE "stored"
 * blocks, which are literally raw bytes with a length header, so a conforming
 * PNG can be written with no compressor at all. The files are two to three times
 * larger than a compressed PNG and that is irrelevant for a capture that exists
 * to be looked at once.
 *
 * Writes 8-bit RGB, non-interlaced.
 */
#ifndef TG_IMG_PNG_H
#define TG_IMG_PNG_H

#include "../src/core/core_types.h"

/* `rgb` is width*height*3 bytes, row-major, top row first. */
TgResult png_write_rgb(const char *path, const u8 *rgb, u32 width, u32 height);

#endif /* TG_IMG_PNG_H */
