#include "ofl_tiffdecode.h"
#include <tiffio.h>
#include <stdlib.h>
#include <string.h>

gboolean ofl_is_tiff_path(const char *path) {
  if (!path) return FALSE;
  const char *ext = strrchr(path, '.');
  if (!ext) return FALSE;
  ext++;
  return (g_ascii_strcasecmp(ext, "tif") == 0 || g_ascii_strcasecmp(ext, "tiff") == 0);
}

static void silent_tiff_warning(const char *module, const char *fmt, va_list ap) {
  (void)module;
  (void)fmt;
  (void)ap;
}

OflRgb16* ofl_tiffdecode_rgb16(const char *path, GError **error) {
  if (!path) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No path provided");
    return NULL;
  }

  // Silence non-critical TIFF warnings (e.g. unknown private tags from scanners)
  TIFFSetWarningHandler(silent_tiff_warning);

  TIFF *tif = TIFFOpen(path, "r");
  if (!tif) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open TIFF file: %s", path);
    return NULL;
  }

  uint32_t w = 0, h = 0;
  uint16_t bps = 8, spp = 3, photo = 2, plan = 1;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &plan);
  int is_tiled = TIFFIsTiled(tif);

  if (w <= 0 || h <= 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid TIFF dimensions (%ux%u)", w, h);
    TIFFClose(tif);
    return NULL;
  }

  size_t npx = (size_t)w * (size_t)h;
  OflRgb16 *out = calloc(1, sizeof(OflRgb16));
  out->width = (int)w;
  out->height = (int)h;
  out->full_width = (int)w;
  out->full_height = (int)h;
  out->channels = 3;
  out->bits = 16;
  out->rgb = malloc(npx * 3 * sizeof(uint16_t));
  if (!out->rgb) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Out of memory allocating TIFF buffer");
    free(out);
    TIFFClose(tif);
    return NULL;
  }

  // Fast path for native 16-bit RGB/RGBA stripped images
  gboolean native_16_read = FALSE;
  if (bps == 16 && (spp == 3 || spp == 4) && photo == PHOTOMETRIC_RGB && plan == PLANARCONFIG_CONTIG && !is_tiled) {
    tsize_t scanline_size = TIFFScanlineSize(tif);
    tdata_t sbuf = _TIFFmalloc(scanline_size);
    if (sbuf) {
      native_16_read = TRUE;
      for (uint32_t row = 0; row < h; row++) {
        if (TIFFReadScanline(tif, sbuf, row, 0) < 0) {
          native_16_read = FALSE;
          break;
        }
        const uint16_t *src = (const uint16_t*)sbuf;
        uint16_t *dst = out->rgb + (size_t)row * (size_t)w * 3;
        for (uint32_t col = 0; col < w; col++) {
          dst[col * 3 + 0] = src[col * spp + 0];
          dst[col * 3 + 1] = src[col * spp + 1];
          dst[col * 3 + 2] = src[col * spp + 2];
        }
      }
      _TIFFfree(sbuf);
    }
  }

  // Universal fallback: handles tiled, 8-bit, grayscale, palette, CMYK, and non-contig formats
  if (!native_16_read) {
    uint32_t *raster = (uint32_t*)_TIFFmalloc(npx * sizeof(uint32_t));
    if (!raster) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Out of memory allocating TIFF raster");
      ofl_rgb16_free(out);
      TIFFClose(tif);
      return NULL;
    }

    if (!TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "TIFFReadRGBAImageOriented failed");
      _TIFFfree(raster);
      ofl_rgb16_free(out);
      TIFFClose(tif);
      return NULL;
    }

    // Convert 8-bit RGBA raster to 16-bit RGB
    for (size_t i = 0; i < npx; i++) {
      uint32_t px = raster[i];
      uint16_t r = TIFFGetR(px);
      uint16_t g = TIFFGetG(px);
      uint16_t b = TIFFGetB(px);
      out->rgb[i * 3 + 0] = (r << 8) | r;
      out->rgb[i * 3 + 1] = (g << 8) | g;
      out->rgb[i * 3 + 2] = (b << 8) | b;
    }

    _TIFFfree(raster);
  }

  TIFFClose(tif);
  return out;
}
