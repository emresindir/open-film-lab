#include "ofl_negpipe.h"
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <glib.h>

static inline float clampf(float x, float a, float b) {
  return x < a ? a : (x > b ? b : x);
}

// Sample a thin border and pick high percentile-ish via max-mean.
// Simple MVP: average of top-ish border pixels tends to approximate film base.
void ofl_neg_estimate_base_from_border(const uint16_t *rgb, int w, int h, float out_base_rgb[3]) {
  // border thickness: ~2% of min dimension, at least 6px
  int t = (int)(0.02f * (float)(w < h ? w : h));
  if (t < 6) t = 6;
  if (t > 64) t = 64;

  double sum[3] = {0,0,0};
  double cnt = 0;

  // Top + Bottom
  for (int y = 0; y < t; y++) {
    for (int x = 0; x < w; x++) {
      const uint16_t *px = &rgb[(y*w + x)*3];
      sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
      cnt++;
    }
  }
  for (int y = h - t; y < h; y++) {
    if (y < 0) continue;
    for (int x = 0; x < w; x++) {
      const uint16_t *px = &rgb[(y*w + x)*3];
      sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
      cnt++;
    }
  }
  // Left + Right
  for (int y = t; y < h - t; y++) {
    for (int x = 0; x < t; x++) {
      const uint16_t *px = &rgb[(y*w + x)*3];
      sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
      cnt++;
    }
    for (int x = w - t; x < w; x++) {
      if (x < 0) continue;
      const uint16_t *px = &rgb[(y*w + x)*3];
      sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
      cnt++;
    }
  }

  double avg[3] = { sum[0]/cnt, sum[1]/cnt, sum[2]/cnt };

  // normalize to 0..1
  out_base_rgb[0] = (float)(avg[0] / 65535.0);
  out_base_rgb[1] = (float)(avg[1] / 65535.0);
  out_base_rgb[2] = (float)(avg[2] / 65535.0);

  // avoid zeros
  for (int c=0;c<3;c++) out_base_rgb[c] = clampf(out_base_rgb[c], 0.0001f, 1.0f);
}

// Compute simple per-channel density min/max using robust-ish percentiles via coarse histogram.
static void density_black_white(const uint16_t *rgb, int w, int h,
                                const float base[3], float out_black[3], float out_white[3]) {
  const int bins = 2048;
  const float dmin = -5.0f, dmax = 8.0f;
  const float inv_range = (float)bins / (dmax - dmin);
  const float eps = 1e-6f;

  // Precompute 16-bit to bin LUT for each channel (<1 ms)
  uint16_t *lut_bin = g_new(uint16_t, 3 * 65536);
  #define LUT_BIN(c, v) lut_bin[(size_t)(c) * 65536 + (size_t)(v)]

  for (int c = 0; c < 3; c++) {
    float b = clampf(base[c], 0.0001f, 1.0f);
    float log_b = logf(b);
    for (int v = 0; v < 65536; v++) {
      float x = (float)v / 65535.0f;
      x = clampf(x, eps, 1.0f);
      float d = log_b - logf(x);
      d = clampf(d, dmin, dmax - 1e-6f);
      int bin = (int)((d - dmin) * inv_range);
      LUT_BIN(c, v) = (uint16_t)CLAMP(bin, 0, bins - 1);
    }
  }

  uint32_t *hist = g_new0(uint32_t, (gsize)bins * 3);
  #define HIST(c, b) hist[(gsize)(c) * (gsize)bins + (gsize)(b)]

  size_t npx = (size_t)w * (size_t)h;
  // Subsample every 4th pixel if image is large (>1M px); statistically identical percentile results
  size_t step = (npx > 1000000) ? 4 : 1;
  for (size_t i = 0; i < npx; i += step) {
    const uint16_t *px = &rgb[i * 3];
    HIST(0, LUT_BIN(0, px[0]))++;
    HIST(1, LUT_BIN(1, px[1]))++;
    HIST(2, LUT_BIN(2, px[2]))++;
  }

  // percentiles
  const float p_black = 0.01f;
  const float p_white = 0.99f;

  for (int c = 0; c < 3; c++) {
    uint64_t total = 0;
    for (int b = 0; b < bins; b++) total += HIST(c, b);

    uint64_t tb = (uint64_t)(p_black * (double)total);
    uint64_t tw = (uint64_t)(p_white * (double)total);

    uint64_t acc = 0;
    int b_black = 0, b_white = bins - 1;

    for (int b = 0; b < bins; b++) {
      acc += HIST(c, b);
      if (acc >= tb) { b_black = b; break; }
    }
    acc = 0;
    for (int b = 0; b < bins; b++) {
      acc += HIST(c, b);
      if (acc >= tw) { b_white = b; break; }
    }

    float black = dmin + ((float)b_black + 0.5f) * (dmax - dmin) / (float)bins;
    float white = dmin + ((float)b_white + 0.5f) * (dmax - dmin) / (float)bins;

    // safety
    if (white - black < 0.2f) white = black + 0.2f;

    out_black[c] = black;
    out_white[c] = white;
  }

  g_free(hist);
  g_free(lut_bin);
  #undef HIST
  #undef LUT_BIN
}

guint8* ofl_neg_convert_to_rgba8(const uint16_t *rgb, int w, int h,
                                const OflNegParams *p,
                                int *out_w, int *out_h, int *out_stride) {
  float base[3] = { p->base_rgb[0], p->base_rgb[1], p->base_rgb[2] };
  for (int c=0;c<3;c++) base[c] = clampf(base[c], 0.0001f, 1.0f);

  float black[3], white[3];
  density_black_white(rgb, w, h, base, black, white);

  // rotation output dimensions
  int rot = ((p->rotate_steps % 4) + 4) % 4;
  int W = (rot % 2 == 0) ? w : h;
  int H = (rot % 2 == 0) ? h : w;

  const int stride = W * 4;
  const size_t total = (size_t)stride * (size_t)H;
  guint8 *out = g_malloc(total);

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;
  if (out_stride) *out_stride = stride;

  const float eps = 1e-6f;
  const float gamma = (p->gamma > 0.01f) ? p->gamma : (1.0f/2.2f);

  // Precompute gains for White Balance (Temperature & Tint) and Exposure (+- 2 stops)
  float temp_norm = clampf(p->temperature / 100.0f, -1.0f, 1.0f);
  float tint_norm = clampf(p->tint / 100.0f, -1.0f, 1.0f);
  float exp_ev    = clampf(p->exposure, -2.0f, 2.0f);
  float c_norm    = clampf(p->contrast / 100.0f, -1.0f, 1.0f);
  float b_offset  = clampf(p->brightness / 100.0f, -1.0f, 1.0f) * 0.5f;

  float exp_mult = powf(2.0f, exp_ev);
  float wb_gain[3];
  wb_gain[0] = powf(2.0f,  temp_norm * 0.5f); // Red: warm boost, cool reduce
  wb_gain[1] = powf(2.0f, -tint_norm * 0.5f); // Green: green boost (-tint), magenta reduce (+tint)
  wb_gain[2] = powf(2.0f, -temp_norm * 0.5f); // Blue: cool boost, warm reduce

  float c_factor = powf(2.0f, c_norm);

  // Precompute 16-bit to 8-bit LUT per channel: turns 78M transcendentals into 3 array lookups per pixel
  uint8_t lut[3][65536];
  for (int c = 0; c < 3; c++) {
    float b = clampf(base[c], 0.0001f, 1.0f);
    float log_b = logf(b);
    float span = white[c] - black[c];
    if (span < 0.01f) span = 0.01f;

    float channel_mult = wb_gain[c] * exp_mult;

    for (int v = 0; v < 65536; v++) {
      float x = (float)v / 65535.0f;
      x = clampf(x, eps, 1.0f);
      float d = log_b - logf(x);
      float t = (d - black[c]) / span;
      t = clampf(t, 0.0f, 1.0f);

      // Linear positive light adjustments (WB & Exposure)
      float t_lin = t * channel_mult;
      t_lin = clampf(t_lin, 0.0f, 1.0f);

      // Apply display gamma (linear -> perceptual)
      float v_p = powf(t_lin, gamma);

      // Contrast adjustment (pivot around middle gray 0.5)
      v_p = (v_p - 0.5f) * c_factor + 0.5f;

      // Brightness adjustment (shift perceptual tone)
      v_p = v_p + b_offset;

      v_p = clampf(v_p, 0.0f, 1.0f);
      lut[c][v] = (uint8_t)(v_p * 255.0f + 0.5f);
    }
  }

  // Fast unrotated path
  if (rot == 0) {
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
      const uint16_t *px = &rgb[i * 3];
      guint8 *dst = &out[i * 4];
      dst[0] = lut[0][px[0]];
      dst[1] = lut[1][px[1]];
      dst[2] = lut[2][px[2]];
      dst[3] = 255;
    }
    return out;
  }

  // Rotated path (CW steps)
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint16_t *px = &rgb[(y * w + x) * 3];
      int X = 0, Y = 0;
      switch (rot) {
        case 1: X = h - 1 - y; Y = x;         break; // 90 CW
        case 2: X = w - 1 - x; Y = h - 1 - y; break; // 180
        case 3: X = y;         Y = w - 1 - x; break; // 270 CW
      }
      guint8 *dst = &out[Y * stride + X * 4];
      dst[0] = lut[0][px[0]];
      dst[1] = lut[1][px[1]];
      dst[2] = lut[2][px[2]];
      dst[3] = 255;
    }
  }

  return out;
}

guint8* ofl_neg_convert_to_rgb8(const uint16_t *rgb, int w, int h,
                               const OflNegParams *p,
                               int *out_w, int *out_h) {
  float base[3] = { p->base_rgb[0], p->base_rgb[1], p->base_rgb[2] };
  for (int c = 0; c < 3; c++) base[c] = clampf(base[c], 0.0001f, 1.0f);

  float black[3], white[3];
  density_black_white(rgb, w, h, base, black, white);

  int rot = ((p->rotate_steps % 4) + 4) % 4;
  int W = (rot % 2 == 0) ? w : h;
  int H = (rot % 2 == 0) ? h : w;

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;

  guint8 *out = g_malloc((size_t)W * (size_t)H * 3);

  const float eps = 1e-6f;
  const float gamma = (p->gamma > 0.01f) ? p->gamma : (1.0f / 2.2f);

  float temp_norm = clampf(p->temperature / 100.0f, -1.0f, 1.0f);
  float tint_norm = clampf(p->tint / 100.0f, -1.0f, 1.0f);
  float exp_ev    = clampf(p->exposure, -2.0f, 2.0f);
  float c_norm    = clampf(p->contrast / 100.0f, -1.0f, 1.0f);
  float b_offset  = clampf(p->brightness / 100.0f, -1.0f, 1.0f) * 0.5f;

  float exp_mult = powf(2.0f, exp_ev);
  float wb_gain[3];
  wb_gain[0] = powf(2.0f,  temp_norm * 0.5f);
  wb_gain[1] = powf(2.0f, -tint_norm * 0.5f);
  wb_gain[2] = powf(2.0f, -temp_norm * 0.5f);

  float c_factor = powf(2.0f, c_norm);

  uint8_t lut[3][65536];
  for (int c = 0; c < 3; c++) {
    float b = clampf(base[c], 0.0001f, 1.0f);
    float log_b = logf(b);
    float span = white[c] - black[c];
    if (span < 0.01f) span = 0.01f;

    float channel_mult = wb_gain[c] * exp_mult;

    for (int v = 0; v < 65536; v++) {
      float x = (float)v / 65535.0f;
      x = clampf(x, eps, 1.0f);
      float d = log_b - logf(x);
      float t = (d - black[c]) / span;
      t = clampf(t, 0.0f, 1.0f);

      float t_lin = t * channel_mult;
      t_lin = clampf(t_lin, 0.0f, 1.0f);

      float v_p = powf(t_lin, gamma);
      v_p = (v_p - 0.5f) * c_factor + 0.5f;
      v_p = v_p + b_offset;
      v_p = clampf(v_p, 0.0f, 1.0f);

      lut[c][v] = (uint8_t)(v_p * 255.0f + 0.5f);
    }
  }

  if (rot == 0) {
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
      const uint16_t *px = &rgb[i * 3];
      guint8 *dst = &out[i * 3];
      dst[0] = lut[0][px[0]];
      dst[1] = lut[1][px[1]];
      dst[2] = lut[2][px[2]];
    }
    return out;
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint16_t *px = &rgb[(y * w + x) * 3];
      int X = 0, Y = 0;
      switch (rot) {
        case 1: X = h - 1 - y; Y = x;         break;
        case 2: X = w - 1 - x; Y = h - 1 - y; break;
        case 3: X = y;         Y = w - 1 - x; break;
      }
      guint8 *dst = &out[(Y * W + X) * 3];
      dst[0] = lut[0][px[0]];
      dst[1] = lut[1][px[1]];
      dst[2] = lut[2][px[2]];
    }
  }

  return out;
}

uint16_t* ofl_neg_convert_to_rgb16(const uint16_t *rgb, int w, int h,
                                  const OflNegParams *p,
                                  int *out_w, int *out_h) {
  float base[3] = { p->base_rgb[0], p->base_rgb[1], p->base_rgb[2] };
  for (int c = 0; c < 3; c++) base[c] = clampf(base[c], 0.0001f, 1.0f);

  float black[3], white[3];
  density_black_white(rgb, w, h, base, black, white);

  int rot = ((p->rotate_steps % 4) + 4) % 4;
  int W = (rot % 2 == 0) ? w : h;
  int H = (rot % 2 == 0) ? h : w;

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;

  uint16_t *out = g_malloc((size_t)W * (size_t)H * 3 * sizeof(uint16_t));

  const float eps = 1e-6f;
  const float gamma = (p->gamma > 0.01f) ? p->gamma : (1.0f / 2.2f);

  float temp_norm = clampf(p->temperature / 100.0f, -1.0f, 1.0f);
  float tint_norm = clampf(p->tint / 100.0f, -1.0f, 1.0f);
  float exp_ev    = clampf(p->exposure, -2.0f, 2.0f);
  float c_norm    = clampf(p->contrast / 100.0f, -1.0f, 1.0f);
  float b_offset  = clampf(p->brightness / 100.0f, -1.0f, 1.0f) * 0.5f;

  float exp_mult = powf(2.0f, exp_ev);
  float wb_gain[3];
  wb_gain[0] = powf(2.0f,  temp_norm * 0.5f);
  wb_gain[1] = powf(2.0f, -tint_norm * 0.5f);
  wb_gain[2] = powf(2.0f, -temp_norm * 0.5f);

  float c_factor = powf(2.0f, c_norm);

  uint16_t lut16[3][65536];
  for (int c = 0; c < 3; c++) {
    float b = clampf(base[c], 0.0001f, 1.0f);
    float log_b = logf(b);
    float span = white[c] - black[c];
    if (span < 0.01f) span = 0.01f;

    float channel_mult = wb_gain[c] * exp_mult;

    for (int v = 0; v < 65536; v++) {
      float x = (float)v / 65535.0f;
      x = clampf(x, eps, 1.0f);
      float d = log_b - logf(x);
      float t = (d - black[c]) / span;
      t = clampf(t, 0.0f, 1.0f);

      float t_lin = t * channel_mult;
      t_lin = clampf(t_lin, 0.0f, 1.0f);

      float v_p = powf(t_lin, gamma);
      v_p = (v_p - 0.5f) * c_factor + 0.5f;
      v_p = v_p + b_offset;
      v_p = clampf(v_p, 0.0f, 1.0f);

      lut16[c][v] = (uint16_t)(v_p * 65535.0f + 0.5f);
    }
  }

  if (rot == 0) {
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
      const uint16_t *px = &rgb[i * 3];
      uint16_t *dst = &out[i * 3];
      dst[0] = lut16[0][px[0]];
      dst[1] = lut16[1][px[1]];
      dst[2] = lut16[2][px[2]];
    }
    return out;
  }

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint16_t *px = &rgb[(y * w + x) * 3];
      int X = 0, Y = 0;
      switch (rot) {
        case 1: X = h - 1 - y; Y = x;         break;
        case 2: X = w - 1 - x; Y = h - 1 - y; break;
        case 3: X = y;         Y = w - 1 - x; break;
      }
      uint16_t *dst = &out[(Y * W + X) * 3];
      dst[0] = lut16[0][px[0]];
      dst[1] = lut16[1][px[1]];
      dst[2] = lut16[2][px[2]];
    }
  }

  return out;
}