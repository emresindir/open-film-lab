#pragma once
#include <glib.h>
#include <stdint.h>

typedef struct {
  int width;
  int height;
  int full_width;   // Original sensor width before preview downsampling
  int full_height;  // Original sensor height before preview downsampling
  int channels;     // expect 3
  int bits;         // expect 16
  uint16_t *rgb;    // interleaved RGBRGB... size = w*h*3
} OflRgb16;

void ofl_rgb16_free(OflRgb16 *img);

// Decode a RAW file into 16-bit RGB preview.
// This uses LibRaw processing with settings chosen to avoid "helpful" auto stuff.
OflRgb16* ofl_rawdecode_rgb16_preview(const char *path, GError **error);

// Decode a RAW file into full-resolution 16-bit RGB without downsampling for export.
// Uses high-quality demosaicing (AHD).
OflRgb16* ofl_rawdecode_rgb16_full(const char *path, GError **error);