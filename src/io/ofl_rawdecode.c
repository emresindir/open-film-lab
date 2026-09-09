#include "ofl_rawdecode.h"
#include <libraw/libraw.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <stdarg.h>

void ofl_rgb16_free(OflRgb16 *img) {
  if (!img) return;
  free(img->rgb);
  free(img);
}

static void set_err(GError **error, const char *fmt, ...) {
  if (!error) return;

  va_list ap;
  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", msg);
  g_free(msg);
}

static OflRgb16* rawdecode_internal(const char *path, gboolean is_preview, GError **error) {
  libraw_data_t *raw = libraw_init(0);
  if (!raw) {
    set_err(error, "libraw_init failed");
    return NULL;
  }

  int rc = libraw_open_file(raw, path);
  if (rc != LIBRAW_SUCCESS) {
    set_err(error, "libraw_open_file failed: %s", libraw_strerror(rc));
    libraw_close(raw);
    return NULL;
  }

  // Unpack RAW data
  rc = libraw_unpack(raw);
  if (rc != LIBRAW_SUCCESS) {
    set_err(error, "libraw_unpack failed: %s", libraw_strerror(rc));
    libraw_close(raw);
    return NULL;
  }

  // ---- Parameters: output 16-bit, linear response (gamma 1.0) ----
  raw->params.output_bps = 16;
  raw->params.no_auto_bright = 0; // scale sensor values to full 16-bit range [0, 65535]
  raw->params.use_auto_wb = 0;
  raw->params.use_camera_wb = 1; // Use camera recorded white balance

  // Keep response close to linear. (LibRaw expects gamm[0], gamm[1])
  raw->params.gamm[0] = 1.0f;
  raw->params.gamm[1] = 1.0f;

  raw->params.half_size = 0;
  raw->params.user_qual = is_preview ? 0 : 3; // 0=fast linear for preview, 3=AHD high quality for export

  // Output color space: 0 = raw camera color space with camera WB applied.
  // This preserves the full blue channel and film base orange mask without applying
  // daylight positive matrices that subtract green from blue and crush negatives.
  raw->params.output_color = 0;

  // Process (demosaic + basic color transform)
  rc = libraw_dcraw_process(raw);
  if (rc != LIBRAW_SUCCESS) {
    set_err(error, "libraw_dcraw_process failed: %s", libraw_strerror(rc));
    libraw_close(raw);
    return NULL;
  }

  // Extract processed image to memory
  libraw_processed_image_t *img = libraw_dcraw_make_mem_image(raw, &rc);
  if (!img || rc != LIBRAW_SUCCESS) {
    set_err(error, "libraw_dcraw_make_mem_image failed: %s", libraw_strerror(rc));
    if (img) libraw_dcraw_clear_mem(img);
    libraw_close(raw);
    return NULL;
  }

  if (img->type != LIBRAW_IMAGE_BITMAP || img->colors < 3 || img->bits != 16) {
    set_err(error, "Unexpected processed image format (type=%d colors=%d bits=%d)",
            img->type, img->colors, img->bits);
    libraw_dcraw_clear_mem(img);
    libraw_close(raw);
    return NULL;
  }

  OflRgb16 *out = calloc(1, sizeof(OflRgb16));
  out->width = (int)img->width;
  out->height = (int)img->height;
  out->full_width = (int)img->width;
  out->full_height = (int)img->height;
  out->channels = 3;
  out->bits = 16;

  size_t npx = (size_t)out->width * (size_t)out->height;
  out->rgb = malloc(npx * 3 * sizeof(uint16_t));

  // LibRaw bitmap is interleaved RGBRGB... for colors>=3
  memcpy(out->rgb, img->data, npx * 3 * sizeof(uint16_t));

  libraw_dcraw_clear_mem(img);
  libraw_close(raw);

  // For interactive preview only: downsample large images (>3200x2400) by 2x using box filtering
  // to save memory and ensure snappy redraws. Full export skips this to retain 100% full resolution.
  if (is_preview && (out->width > 3200 || out->height > 2400)) {
    int dw = out->width / 2;
    int dh = out->height / 2;
    uint16_t *down = malloc((size_t)dw * (size_t)dh * 3 * sizeof(uint16_t));
    if (down) {
      int sw = out->width;
      for (int y = 0; y < dh; y++) {
        const uint16_t *r0 = &out->rgb[(y * 2) * sw * 3];
        const uint16_t *r1 = &out->rgb[(y * 2 + 1) * sw * 3];
        uint16_t *d = &down[y * dw * 3];
        for (int x = 0; x < dw; x++) {
          int sx = x * 2 * 3;
          d[x*3 + 0] = (uint16_t)((r0[sx + 0] + r0[sx + 3] + r1[sx + 0] + r1[sx + 3] + 2) >> 2);
          d[x*3 + 1] = (uint16_t)((r0[sx + 1] + r0[sx + 4] + r1[sx + 1] + r1[sx + 4] + 2) >> 2);
          d[x*3 + 2] = (uint16_t)((r0[sx + 2] + r0[sx + 5] + r1[sx + 2] + r1[sx + 5] + 2) >> 2);
        }
      }
      free(out->rgb);
      out->rgb = down;
      out->width = dw;
      out->height = dh;
    }
  }

  return out;
}

OflRgb16* ofl_rawdecode_rgb16_preview(const char *path, GError **error) {
  return rawdecode_internal(path, TRUE, error);
}

OflRgb16* ofl_rawdecode_rgb16_full(const char *path, GError **error) {
  return rawdecode_internal(path, FALSE, error);
}