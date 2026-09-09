#pragma once
#include "ofl_rawdecode.h"
#include <glib.h>
#include <gio/gio.h>

// Check if a path or filename has a TIFF extension (.tif or .tiff, case-insensitive)
gboolean ofl_is_tiff_path(const char *path);

// Decode a TIFF image file into an OflRgb16 structure (16-bit RGB)
OflRgb16* ofl_tiffdecode_rgb16(const char *path, GError **error);
