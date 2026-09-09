#pragma once
#include <gtk/gtk.h>

// Returns a GdkTexture made from embedded JPEG thumbnail if available.
// On failure returns NULL and sets error.
GdkTexture* ofl_rawthumb_texture_from_file(const char *path, GError **error);