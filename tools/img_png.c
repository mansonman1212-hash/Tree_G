#include "img_png.h"

#include "../src/core/log.h"
#include "../src/core/mem.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Checksums                                                                 */
/* ------------------------------------------------------------------------- */

/* Incremental CRC-32. Incremental rather than one-shot so a chunk's CRC can span
 * its type and its payload without allocating a copy of the whole image. */
static u32 crc32_update(u32 state, const u8 *data, u64 len) {
    static u32 table[256];
    static bool built = false;
    u64 i;

    if (!built) {
        u32 n, k;
        for (n = 0; n < 256; ++n) {
            u32 c = n;
            for (k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        built = true;
    }
    for (i = 0; i < len; ++i) {
        state = table[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

static u32 adler32_of(const u8 *data, u64 len) {
    u32 a = 1, b = 0;
    u64 i;
    for (i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------------- */
/* Chunk writing                                                             */
/* ------------------------------------------------------------------------- */

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static bool write_chunk(FILE *f, const char *type, const u8 *data, u32 len) {
    u8 hdr[8];
    u8 crcbuf[4];
    u32 crc;

    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) { return false; }
    if (len > 0 && fwrite(data, 1, len, f) != len) { return false; }

    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const u8 *)type, 4);
    if (len > 0) { crc = crc32_update(crc, data, len); }
    crc ^= 0xFFFFFFFFu;
    put_be32(crcbuf, crc);
    return fwrite(crcbuf, 1, 4, f) == 4;
}

/* ------------------------------------------------------------------------- */
/* Writer                                                                    */
/* ------------------------------------------------------------------------- */

TgResult png_write_rgb(const char *path, const u8 *rgb, u32 width, u32 height) {
    static const u8 signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    FILE *f;
    u8 ihdr[13];
    u8 *raw = NULL;      /* filtered scanlines: 1 filter byte + RGB per row   */
    u8 *zstream = NULL;  /* zlib wrapper around stored deflate blocks         */
    u64 raw_len, z_len, z_cap;
    u64 y;
    TgResult result = TG_OK;

    if (path == NULL || rgb == NULL || width == 0 || height == 0) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (!tg_ckd_mul_u64((u64)width, 3u, &raw_len)) { return TG_ERR_OVERFLOW; }
    if (!tg_ckd_add_u64(raw_len, 1u, &raw_len)) { return TG_ERR_OVERFLOW; }
    if (!tg_ckd_mul_u64(raw_len, (u64)height, &raw_len)) { return TG_ERR_OVERFLOW; }

    raw = (u8 *)tg_alloc(raw_len);
    if (raw == NULL) { return TG_ERR_OUT_OF_MEMORY; }

    for (y = 0; y < height; ++y) {
        u8 *dst = raw + y * ((u64)width * 3u + 1u);
        dst[0] = 0; /* filter type 0: none */
        memcpy(dst + 1, rgb + y * (u64)width * 3u, (size_t)width * 3u);
    }

    /* zlib stream: 2-byte header, stored deflate blocks of at most 65535 bytes,
     * 4-byte Adler-32 of the uncompressed data. */
    {
        u64 blocks = (raw_len + 65534u) / 65535u;
        if (blocks == 0) { blocks = 1; }
        z_cap = 2u + blocks * 5u + raw_len + 4u;
        zstream = (u8 *)tg_alloc(z_cap);
        if (zstream == NULL) {
            result = TG_ERR_OUT_OF_MEMORY;
            goto done;
        }
    }
    z_len = 0;
    zstream[z_len++] = 0x78; /* CM=8 (deflate), CINFO=7 (32K window)          */
    zstream[z_len++] = 0x01; /* FCHECK so that (0x78<<8|0x01) % 31 == 0        */
    {
        u64 offset = 0;
        while (offset < raw_len) {
            u64 remaining = raw_len - offset;
            u32 chunk = (remaining > 65535u) ? 65535u : (u32)remaining;
            bool final_block = (offset + chunk >= raw_len);
            zstream[z_len++] = final_block ? 1u : 0u; /* BFINAL, BTYPE=stored */
            zstream[z_len++] = (u8)(chunk & 0xFFu);
            zstream[z_len++] = (u8)(chunk >> 8);
            zstream[z_len++] = (u8)(~chunk & 0xFFu);
            zstream[z_len++] = (u8)((~chunk >> 8) & 0xFFu);
            memcpy(zstream + z_len, raw + offset, chunk);
            z_len += chunk;
            offset += chunk;
        }
    }
    put_be32(zstream + z_len, adler32_of(raw, raw_len));
    z_len += 4;

    f = fopen(path, "wb");
    if (f == NULL) {
        TG_LOG_ERRORF("png", "cannot open '%s' for writing", path);
        result = TG_ERR_IO;
        goto done;
    }

    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;   /* bit depth                                              */
    ihdr[9] = 2;   /* colour type 2: truecolour RGB                          */
    ihdr[10] = 0;  /* compression: deflate                                   */
    ihdr[11] = 0;  /* filter method 0                                        */
    ihdr[12] = 0;  /* no interlace                                           */

    if (fwrite(signature, 1, 8, f) != 8 ||
        !write_chunk(f, "IHDR", ihdr, 13) ||
        !write_chunk(f, "IDAT", zstream, (u32)z_len) ||
        !write_chunk(f, "IEND", NULL, 0)) {
        TG_LOG_ERRORF("png", "write failed for '%s'", path);
        result = TG_ERR_IO;
    }
    if (fclose(f) != 0 && result == TG_OK) { result = TG_ERR_IO; }

done:
    if (raw != NULL) { tg_free(raw, raw_len); }
    if (zstream != NULL) { tg_free(zstream, z_cap); }
    return result;
}
