/*
 * wzimg.c — WebZero image optimizer (build-time tool)
 *
 * Usage:
 *   wzimg <input> <output.jpg> <width> [quality]
 *
 *   input   : JPEG, PNG, BMP, TGA, GIF, PSD, HDR, PIC, PNM
 *   output  : always JPEG
 *   width   : target width in pixels (height scaled to preserve aspect ratio)
 *   quality : JPEG quality 1-100, default 82
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -Wpedantic -o wzimg tools/wzimg.c
 *   (requires third_party/stb_image.h and third_party/stb_image_write.h)
 *
 * Constraints met:
 *   - Pure C99, zero external dependencies at runtime
 *   - stb headers are vendored single-file implementations (compiled in)
 *   - Bilinear resize implemented from scratch — no external resize library
 *   - malloc is acceptable for a build-time tool (not the serving hot-path)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Vendored stb headers (compiled into this translation unit only)     */
/* ------------------------------------------------------------------ */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_SIMD
#include "../third_party/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

/* ------------------------------------------------------------------ */
/* Bilinear resize — pure C99, no dependencies                         */
/* ------------------------------------------------------------------ */

/*
 * Bilinear sample: given a source image (src, sw, sh, channels),
 * sample at fractional position (fx, fy) and write to `out`.
 * All channels are processed independently.
 */
static void bilinear_sample(
    const unsigned char *src, int sw, int sh, int ch,
    float fx, float fy,
    unsigned char *out)
{
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    /* Clamp to image bounds */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= sw) x1 = sw - 1;
    if (y1 >= sh) y1 = sh - 1;

    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    for (int c = 0; c < ch; c++) {
        float p00 = (float)src[(y0 * sw + x0) * ch + c];
        float p10 = (float)src[(y0 * sw + x1) * ch + c];
        float p01 = (float)src[(y1 * sw + x0) * ch + c];
        float p11 = (float)src[(y1 * sw + x1) * ch + c];

        float top    = p00 + tx * (p10 - p00);
        float bottom = p01 + tx * (p11 - p01);
        float v      = top  + ty * (bottom - top);

        int iv = (int)(v + 0.5f);
        if (iv < 0)   iv = 0;
        if (iv > 255) iv = 255;
        out[c] = (unsigned char)iv;
    }
}

/*
 * Resize src (sw×sh, ch channels) to dst_w×dst_h using bilinear interpolation.
 * Returns a heap-allocated buffer of dst_w * dst_h * ch bytes, or NULL on OOM.
 * Caller must free() the result.
 */
static unsigned char *resize_bilinear(
    const unsigned char *src, int sw, int sh, int ch,
    int dst_w, int dst_h)
{
    unsigned char *dst = (unsigned char *)malloc((size_t)dst_w * (size_t)dst_h * (size_t)ch);
    if (!dst) return NULL;

    float sx = (float)sw / (float)dst_w;
    float sy = (float)sh / (float)dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        float fy = ((float)dy + 0.5f) * sy - 0.5f;
        for (int dx = 0; dx < dst_w; dx++) {
            float fx = ((float)dx + 0.5f) * sx - 0.5f;
            bilinear_sample(src, sw, sh, ch, fx, fy,
                            dst + (dy * dst_w + dx) * ch);
        }
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
            "Usage: wzimg <input> <output.jpg> <width> [quality=82]\n"
            "\n"
            "  Resizes <input> to <width> pixels wide (aspect-ratio preserved),\n"
            "  outputs as JPEG.\n"
            "\n"
            "  Supported input formats: JPEG, PNG, BMP, GIF\n"
            "  quality: 1-100 (default 82)\n");
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];
    char *end;
    long parsed_width, parsed_quality = 82;
    errno = 0; parsed_width = strtol(argv[3], &end, 10);
    if (errno || !argv[3][0] || *end || parsed_width < 1 || parsed_width > 16383) {
        fprintf(stderr, "wzimg: width must be an integer from 1 to 16383\n"); return 1;
    }
    if (argc == 5) {
        errno = 0; parsed_quality = strtol(argv[4], &end, 10);
        if (errno || !argv[4][0] || *end || parsed_quality < 1 || parsed_quality > 100) {
            fprintf(stderr, "wzimg: quality must be an integer from 1 to 100\n"); return 1;
        }
    }
    int dst_w = (int)parsed_width, quality = (int)parsed_quality;

    if (dst_w <= 0 || dst_w > 65535) {
        fprintf(stderr, "wzimg: invalid width '%s'\n", argv[3]);
        return 1;
    }
    if (quality < 1 || quality > 100) {
        fprintf(stderr, "wzimg: quality must be 1-100, got %d\n", quality);
        return 1;
    }

    /* Decode input image */
    int src_w = 0, src_h = 0, src_ch = 0;
    if (!stbi_info(in_path, &src_w, &src_h, &src_ch) || src_w <= 0 || src_h <= 0 ||
        src_w > 16384 || src_h > 16384 || (uint64_t)src_w * (uint64_t)src_h > 64u * 1024u * 1024u) {
        fprintf(stderr, "wzimg: invalid image or dimensions exceed 16384 / 64 megapixels\n"); return 1;
    }
    unsigned char *src = stbi_load(in_path, &src_w, &src_h, &src_ch, 0);
    if (!src) {
        fprintf(stderr, "wzimg: cannot load '%s': %s\n",
                in_path, stbi_failure_reason());
        return 1;
    }

    /* Force RGB (drop alpha — JPEG has no alpha channel) */
    unsigned char *rgb = src;
    int            out_ch = src_ch;
    if (src_ch == 4) {
        /* Flatten alpha over white background */
        rgb = (unsigned char *)malloc((size_t)src_w * (size_t)src_h * 3u);
        if (!rgb) {
            fprintf(stderr, "wzimg: OOM during alpha flatten\n");
            stbi_image_free(src);
            return 1;
        }
        for (int i = 0; i < src_w * src_h; i++) {
            unsigned int a  = src[i * 4 + 3];
            unsigned int ia = 255 - a;
            rgb[i * 3 + 0] = (unsigned char)((src[i * 4 + 0] * a + 255 * ia) / 255);
            rgb[i * 3 + 1] = (unsigned char)((src[i * 4 + 1] * a + 255 * ia) / 255);
            rgb[i * 3 + 2] = (unsigned char)((src[i * 4 + 2] * a + 255 * ia) / 255);
        }
        out_ch = 3;
    } else if (src_ch == 2) {
        /* Gray + alpha → gray */
        rgb = (unsigned char *)malloc((size_t)src_w * (size_t)src_h);
        if (!rgb) {
            fprintf(stderr, "wzimg: OOM\n");
            stbi_image_free(src);
            return 1;
        }
        for (int i = 0; i < src_w * src_h; i++) {
            unsigned int a  = src[i * 2 + 1];
            unsigned int ia = 255 - a;
            rgb[i] = (unsigned char)((src[i * 2 + 0] * a + 255 * ia) / 255);
        }
        out_ch = 1;
    }

    /* If already at or below target width, just re-encode (no upscaling) */
    if (dst_w >= src_w) {
        fprintf(stderr,
            "wzimg: source width %d <= target %d, encoding without resize\n",
            src_w, dst_w);
        int ok = stbi_write_jpg(out_path, src_w, src_h, out_ch, rgb, quality);
        if (rgb != src) free(rgb);
        stbi_image_free(src);
        if (!ok) {
            fprintf(stderr, "wzimg: failed to write '%s'\n", out_path);
            return 1;
        }
        fprintf(stderr, "wzimg: %s → %s  (%dx%d, q=%d)\n",
                in_path, out_path, src_w, src_h, quality);
        return 0;
    }

    /* Compute output height preserving aspect ratio */
    int dst_h = (int)((float)src_h * (float)dst_w / (float)src_w + 0.5f);
    if (dst_h < 1) dst_h = 1;

    /* Bilinear resize */
    unsigned char *resized = resize_bilinear(rgb, src_w, src_h, out_ch,
                                              dst_w, dst_h);
    if (!resized) {
        fprintf(stderr, "wzimg: OOM during resize\n");
        if (rgb != src) free(rgb);
        stbi_image_free(src);
        return 1;
    }

    /* Write JPEG */
    int ok = stbi_write_jpg(out_path, dst_w, dst_h, out_ch, resized, quality);
    free(resized);
    if (rgb != src) free(rgb);
    stbi_image_free(src);

    if (!ok) {
        fprintf(stderr, "wzimg: failed to write '%s'\n", out_path);
        return 1;
    }

    fprintf(stderr, "wzimg: %s → %s  (%dx%d → %dx%d, q=%d)\n",
            in_path, out_path, src_w, src_h, dst_w, dst_h, quality);
    return 0;
}
