#pragma once
#include <glib.h>
#include <gio/gio.h>
#include <stdbool.h>
#include "ofl_frame.h"
#include "ofl_negpipe.h"

G_BEGIN_DECLS

typedef enum {
  OFL_EXPORT_FORMAT_JPEG = 0,
  OFL_EXPORT_FORMAT_TIFF = 1
} OflExportFormat;

typedef enum {
  OFL_PROFILE_SRGB = 0,
  OFL_PROFILE_ADOBE_RGB = 1,
  OFL_PROFILE_DISPLAY_P3 = 2
} OflColorProfile;

typedef enum {
  OFL_TIFF_DEPTH_8BIT = 0,
  OFL_TIFF_DEPTH_16BIT = 1
} OflTiffDepth;

typedef struct {
  char *output_dir;          // Target directory path
  char *naming_pattern;      // e.g. "{name}", "{date}_{name}", "{name}_{index}"
  OflExportFormat format;    // JPEG or TIFF
  int jpeg_quality;          // 1 .. 100 (default: 92)
  OflTiffDepth tiff_depth;   // 8-bit or 16-bit
  OflColorProfile profile;   // sRGB, Adobe RGB, Display P3
  gboolean embed_profile;    // TRUE to embed ICC profile
} OflExportSettings;

void ofl_export_settings_init_defaults(OflExportSettings *s);
void ofl_export_settings_clear(OflExportSettings *s);

// Formats destination filename from pattern given source path, index (1-based), and format
char* ofl_export_format_filename(const char *pattern,
                                 const char *src_path,
                                 int item_index,
                                 OflExportFormat format);

// Export a single frame with roll base and per-frame adjustments to disk
gboolean ofl_export_frame(OflFrame *frame,
                          const float roll_base_rgb[3],
                          const OflNegParams *base_neg,
                          const OflExportSettings *settings,
                          int item_index,
                          char **out_saved_path,
                          GError **error);

G_END_DECLS
