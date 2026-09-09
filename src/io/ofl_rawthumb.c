#include "ofl_rawthumb.h"
#include <libraw/libraw.h>

GdkTexture* ofl_rawthumb_texture_from_file(const char *path, GError **error) {
  libraw_data_t *raw = libraw_init(0);
  if (!raw) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "libraw_init failed");
    return NULL;
  }

#ifdef _WIN32
  wchar_t *wpath = (wchar_t *)g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
  if (!wpath) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to convert path to UTF-16: %s", path);
    libraw_close(raw);
    return NULL;
  }
  int rc = libraw_open_wfile(raw, wpath);
  g_free(wpath);
#else
  int rc = libraw_open_file(raw, path);
#endif
  if (rc != LIBRAW_SUCCESS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "libraw_open_file failed: %s", libraw_strerror(rc));
    libraw_close(raw);
    return NULL;
  }

  rc = libraw_unpack_thumb(raw);
  if (rc != LIBRAW_SUCCESS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "libraw_unpack_thumb failed: %s", libraw_strerror(rc));
    libraw_close(raw);
    return NULL;
  }

  if (raw->thumbnail.tformat != LIBRAW_THUMBNAIL_JPEG) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "No embedded JPEG thumbnail (format=%d)", raw->thumbnail.tformat);
    libraw_close(raw);
    return NULL;
  }

  if (!raw->thumbnail.thumb || raw->thumbnail.tlength <= 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Thumbnail bytes unavailable");
    libraw_close(raw);
    return NULL;
  }

  GBytes *bytes = g_bytes_new(raw->thumbnail.thumb, (gsize)raw->thumbnail.tlength);
  GdkTexture *tex = gdk_texture_new_from_bytes(bytes, error);

  g_bytes_unref(bytes);
  libraw_close(raw);
  return tex;
}