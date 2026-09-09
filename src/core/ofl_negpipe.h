#pragma once
#include <glib.h>
#include <stdint.h>

typedef struct {
  float base_rgb[3];      // mask/base removal multiplier (0..1-ish)
  float black[3];         // density black point per channel
  float white[3];         // density white point per channel
  float gamma;            // curve gamma (e.g. 1/2.2-ish)
  int rotate_steps;       // 0,1,2,3 (each = 90° CW)

  // Per-frame adjustments
  float temperature;      // -100.0 to +100.0 (default 0.0)
  float tint;             // -100.0 to +100.0 (default 0.0)
  float exposure;         // -2.0 to +2.0 stops EV (default 0.0)
  float contrast;         // -100.0 to +100.0 (default 0.0)
  float brightness;       // -100.0 to +100.0 (default 0.0)
} OflNegParams;

// Auto-estimate film base from image border (simple MVP heuristic).
void ofl_neg_estimate_base_from_border(const uint16_t *rgb, int w, int h, float out_base_rgb[3]);

// Convert RGB16 negative -> RGBA8 positive with base removal + log inversion + curve + rotation.
// Returns newly allocated buffer (g_malloc). Caller frees with g_free.
guint8* ofl_neg_convert_to_rgba8(const uint16_t *rgb, int w, int h,
                                const OflNegParams *p,
                                int *out_w, int *out_h, int *out_stride);

// Convert RGB16 negative -> RGB8 positive (3-channel) for JPEG and 8-bit TIFF export.
// Returns newly allocated buffer (g_malloc). Caller frees with g_free.
guint8* ofl_neg_convert_to_rgb8(const uint16_t *rgb, int w, int h,
                               const OflNegParams *p,
                               int *out_w, int *out_h);

// Convert RGB16 negative -> RGB16 positive (3-channel, 16-bit) for archival 16-bit TIFF export.
// Returns newly allocated buffer (g_malloc). Caller frees with g_free.
uint16_t* ofl_neg_convert_to_rgb16(const uint16_t *rgb, int w, int h,
                                  const OflNegParams *p,
                                  int *out_w, int *out_h);