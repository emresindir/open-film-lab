#include "ofl_export.h"
#include "ofl_rawdecode.h"
#include "ofl_tiffdecode.h"
#include "ofl_negpipe.h"
#include "ofl_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <jpeglib.h>
#include <tiffio.h>
#include <lcms2.h>
#include <glib/gstdio.h>

void ofl_export_settings_init_defaults(OflExportSettings *s) {
  if (!s) return;
  memset(s, 0, sizeof(*s));
  s->output_dir = NULL;
  s->naming_pattern = g_strdup("{name}");
  s->format = OFL_EXPORT_FORMAT_JPEG;
  s->jpeg_quality = 92;
  s->tiff_depth = OFL_TIFF_DEPTH_16BIT;
  s->profile = OFL_PROFILE_SRGB;
  s->embed_profile = TRUE;
}

void ofl_export_settings_clear(OflExportSettings *s) {
  if (!s) return;
  g_free(s->output_dir);
  g_free(s->naming_pattern);
  memset(s, 0, sizeof(*s));
}

// Check if a path is TIFF
static gboolean is_tiff_path(const char *path) {
  if (!path) return FALSE;
  const char *dot = strrchr(path, '.');
  if (!dot) return FALSE;
  return (g_ascii_strcasecmp(dot, ".tif") == 0 || g_ascii_strcasecmp(dot, ".tiff") == 0);
}

// Generate ICC profile byte buffer and CMS profile handle
static cmsHPROFILE create_cms_profile(OflColorProfile profile) {
  switch (profile) {
    case OFL_PROFILE_ADOBE_RGB: {
      cmsCIExyY D65 = { 0.3127, 0.3290, 1.0 };
      cmsCIExyYTRIPLE AdobePrimaries = {
        { 0.6400, 0.3300, 1.0 }, // Red
        { 0.2100, 0.7100, 1.0 }, // Green
        { 0.1500, 0.0600, 1.0 }  // Blue
      };
      cmsToneCurve *gamma22 = cmsBuildGamma(NULL, 2.19921875);
      cmsToneCurve *curves[3] = { gamma22, gamma22, gamma22 };
      cmsHPROFILE hProfile = cmsCreateRGBProfile(&D65, &AdobePrimaries, curves);
      cmsFreeToneCurve(gamma22);
      return hProfile;
    }
    case OFL_PROFILE_DISPLAY_P3: {
      cmsCIExyY D65 = { 0.3127, 0.3290, 1.0 };
      cmsCIExyYTRIPLE P3Primaries = {
        { 0.6800, 0.3200, 1.0 }, // Red
        { 0.2650, 0.6900, 1.0 }, // Green
        { 0.1500, 0.0600, 1.0 }  // Blue
      };
      cmsToneCurve *srgbCurve = cmsBuildGamma(NULL, 2.2);
      cmsToneCurve *curves[3] = { srgbCurve, srgbCurve, srgbCurve };
      cmsHPROFILE hProfile = cmsCreateRGBProfile(&D65, &P3Primaries, curves);
      cmsFreeToneCurve(srgbCurve);
      return hProfile;
    }
    case OFL_PROFILE_SRGB:
    default:
      return cmsCreate_sRGBProfile();
  }
}

static unsigned char* get_icc_profile_bytes(OflColorProfile profile, cmsUInt32Number *out_len) {
  cmsHPROFILE hProf = create_cms_profile(profile);
  if (!hProf) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  cmsUInt32Number len = 0;
  cmsSaveProfileToMem(hProf, NULL, &len);
  if (len == 0) {
    cmsCloseProfile(hProf);
    if (out_len) *out_len = 0;
    return NULL;
  }
  unsigned char *buf = g_malloc(len);
  cmsSaveProfileToMem(hProf, buf, &len);
  cmsCloseProfile(hProf);
  if (out_len) *out_len = len;
  return buf;
}

// String replace helper
static char* str_replace(const char *orig, const char *rep, const char *with) {
  if (!orig || !rep || !with) return g_strdup(orig ? orig : "");
  char **parts = g_strsplit(orig, rep, -1);
  char *result = g_strjoinv(with, parts);
  g_strfreev(parts);
  return result;
}

char* ofl_export_format_filename(const char *pattern,
                                 const char *src_path,
                                 int item_index,
                                 OflExportFormat format) {
  // Extract base name without extension
  char *base = NULL;
  if (src_path) {
    char *fname = g_path_get_basename(src_path);
    char *dot = strrchr(fname, '.');
    if (dot) *dot = '\0';
    base = fname;
  } else {
    base = g_strdup_printf("frame_%03d", item_index);
  }

  // Get current local time using portable GDateTime
  GDateTime *now = g_date_time_new_now_local();
  int year = 1970, mon = 1, day = 1, hour = 0, min = 0, sec = 0;
  if (now) {
    year = g_date_time_get_year(now);
    mon  = g_date_time_get_month(now);
    day  = g_date_time_get_day_of_month(now);
    hour = g_date_time_get_hour(now);
    min  = g_date_time_get_minute(now);
    sec  = g_date_time_get_second(now);
    g_date_time_unref(now);
  }

  char s_date[32], s_time[32], s_year[16], s_month[16], s_day[16], s_idx[16];
  snprintf(s_date, sizeof(s_date), "%04d-%02d-%02d", year, mon, day);
  snprintf(s_time, sizeof(s_time), "%02d%02d%02d", hour, min, sec);
  snprintf(s_year, sizeof(s_year), "%04d", year);
  snprintf(s_month, sizeof(s_month), "%02d", mon);
  snprintf(s_day, sizeof(s_day), "%02d", day);
  snprintf(s_idx, sizeof(s_idx), "%03d", item_index);

  const char *pat = (pattern && *pattern) ? pattern : "{name}";
  char *cur = g_strdup(pat);

  char *tmp = str_replace(cur, "{name}", base); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{date}", s_date); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{time}", s_time); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{year}", s_year); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{month}", s_month); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{day}", s_day); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{index}", s_idx); g_free(cur); cur = tmp;
  tmp = str_replace(cur, "{seq}", s_idx); g_free(cur); cur = tmp;

  g_free(base);

  // Sanitize illegal filesystem characters
  for (char *p = cur; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
        *p == '?' || *p == '"'  || *p == '<' || *p == '>' || *p == '|') {
      *p = '_';
    }
  }

  // Fallback if empty
  if (cur[0] == '\0') {
    g_free(cur);
    cur = g_strdup_printf("image_%03d", item_index);
  }

  const char *ext = (format == OFL_EXPORT_FORMAT_JPEG) ? ".jpg" : ".tif";
  char *final_name = g_strconcat(cur, ext, NULL);
  g_free(cur);

  return final_name;
}

// Write JPEG file with quality level and embedded ICC profile
static gboolean write_jpeg_file(const char *dest_path,
                                const guint8 *rgb8,
                                int width, int height,
                                int quality,
                                gboolean embed_icc,
                                const unsigned char *icc_data,
                                unsigned int icc_len,
                                GError **error) {
  FILE *fp = g_fopen(dest_path, "wb");
  if (!fp) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to open destination file for writing: %s", dest_path);
    return FALSE;
  }

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, CLAMP(quality, 1, 100), TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  if (embed_icc && icc_data && icc_len > 0) {
    jpeg_write_icc_profile(&cinfo, icc_data, icc_len);
  }

  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = (JSAMPROW)&rgb8[cinfo.next_scanline * width * 3];
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  fclose(fp);
  jpeg_destroy_compress(&cinfo);

  return TRUE;
}

// Write TIFF file with 8-bit or 16-bit depth, LZW compression, and embedded ICC profile
static gboolean write_tiff_file(const char *dest_path,
                                const void *rgb_data,
                                int width, int height,
                                gboolean is_16bit,
                                gboolean embed_icc,
                                const unsigned char *icc_data,
                                unsigned int icc_len,
                                GError **error) {
  TIFF *out = TIFFOpen(dest_path, "w");
  if (!out) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to open TIFF file for writing: %s", dest_path);
    return FALSE;
  }

  TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(out, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, is_16bit ? 16 : 8);
  TIFFSetField(out, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
  TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(out, 0));

  if (embed_icc && icc_data && icc_len > 0) {
    TIFFSetField(out, TIFFTAG_ICCPROFILE, icc_len, icc_data);
  }

  const guint8 *bytes8 = (const guint8*)rgb_data;
  const uint16_t *words16 = (const uint16_t*)rgb_data;

  for (int y = 0; y < height; y++) {
    if (is_16bit) {
      if (TIFFWriteScanline(out, (void*)&words16[y * width * 3], y, 0) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write TIFF scanline %d", y);
        TIFFClose(out);
        return FALSE;
      }
    } else {
      if (TIFFWriteScanline(out, (void*)&bytes8[y * width * 3], y, 0) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to write TIFF scanline %d", y);
        TIFFClose(out);
        return FALSE;
      }
    }
  }

  TIFFClose(out);
  return TRUE;
}

gboolean ofl_export_frame(OflFrame *frame,
                          const float roll_base_rgb[3],
                          const OflNegParams *base_neg,
                          const OflExportSettings *settings,
                          int item_index,
                          char **out_saved_path,
                          GError **error) {
  if (!frame || !settings) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid arguments to export frame");
    return FALSE;
  }

  GFile *file = ofl_frame_get_file(frame);
  if (!file) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Frame has no associated file");
    return FALSE;
  }

  char *src_path = g_file_get_path(file);
  if (!src_path) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not resolve frame file path");
    return FALSE;
  }

  // 1. Decode full resolution image
  GError *dec_err = NULL;
  OflRgb16 *img = NULL;
  if (is_tiff_path(src_path)) {
    img = ofl_tiffdecode_rgb16(src_path, &dec_err);
  } else {
    img = ofl_rawdecode_rgb16_full(src_path, &dec_err);
  }

  if (!img) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to decode %s: %s", src_path, dec_err ? dec_err->message : "unknown error");
    if (dec_err) g_clear_error(&dec_err);
    g_free(src_path);
    return FALSE;
  }

  // 2. Setup negative conversion parameters
  OflNegParams p = *base_neg;
  p.base_rgb[0] = roll_base_rgb[0];
  p.base_rgb[1] = roll_base_rgb[1];
  p.base_rgb[2] = roll_base_rgb[2];
  p.rotate_steps = ofl_frame_get_rotate_steps(frame);

  const OflAdjustments *adj = ofl_frame_get_adjustments(frame);
  if (adj) {
    p.temperature = adj->temperature;
    p.tint = adj->tint;
    p.exposure = adj->exposure;
    p.contrast = adj->contrast;
    p.brightness = adj->brightness;
  }

  // 3. Prepare destination file path
  char *fname = ofl_export_format_filename(settings->naming_pattern, src_path, item_index, settings->format);
  const char *out_dir = (settings->output_dir && *settings->output_dir) ? settings->output_dir : ".";

  // Ensure output directory exists
  if (g_mkdir_with_parents(out_dir, 0755) != 0) {
    g_warning("Could not create directory %s", out_dir);
  }

  char *dest_path = g_build_filename(out_dir, fname, NULL);
  g_free(fname);
  g_free(src_path);

  // 4. Generate ICC Profile data if requested
  cmsUInt32Number icc_len = 0;
  unsigned char *icc_data = NULL;
  if (settings->embed_profile) {
    icc_data = get_icc_profile_bytes(settings->profile, &icc_len);
  }

  // 5. Convert negative to RGB & transform color profile if needed
  gboolean success = FALSE;

  const OflCrop *crop = ofl_frame_get_crop(frame);

  if (settings->format == OFL_EXPORT_FORMAT_JPEG ||
      (settings->format == OFL_EXPORT_FORMAT_TIFF && settings->tiff_depth == OFL_TIFF_DEPTH_8BIT)) {
    int out_w = 0, out_h = 0;
    guint8 *rgb8 = ofl_neg_convert_to_rgb8(img->rgb, img->width, img->height, &p, &out_w, &out_h);
    ofl_rgb16_free(img);

    // Apply Crop if enabled
    if (crop && crop->enabled && crop->width > 0.001f && crop->height > 0.001f) {
      int cx = (int)roundf(crop->x * (float)out_w);
      int cy = (int)roundf(crop->y * (float)out_h);
      int cw = (int)roundf(crop->width * (float)out_w);
      int ch = (int)roundf(crop->height * (float)out_h);
      if (cx < 0) cx = 0;
      if (cy < 0) cy = 0;
      if (cx + cw > out_w) cw = out_w - cx;
      if (cy + ch > out_h) ch = out_h - cy;
      if (cw > 0 && ch > 0 && (cw != out_w || ch != out_h)) {
        guint8 *cropped = g_malloc((size_t)cw * (size_t)ch * 3);
        for (int y = 0; y < ch; y++) {
          memcpy(&cropped[y * cw * 3], &rgb8[((cy + y) * out_w + cx) * 3], (size_t)cw * 3);
        }
        g_free(rgb8);
        rgb8 = cropped;
        out_w = cw;
        out_h = ch;
      }
    }

    // Apply Little CMS transform if target profile is not sRGB
    if (settings->profile != OFL_PROFILE_SRGB) {
      cmsHPROFILE hsRGB = cmsCreate_sRGBProfile();
      cmsHPROFILE hTarget = create_cms_profile(settings->profile);
      if (hsRGB && hTarget) {
        cmsHTRANSFORM xform = cmsCreateTransform(hsRGB, TYPE_RGB_8, hTarget, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
        if (xform) {
          cmsDoTransform(xform, rgb8, rgb8, (cmsUInt32Number)((size_t)out_w * (size_t)out_h));
          cmsDeleteTransform(xform);
        }
      }
      if (hsRGB) cmsCloseProfile(hsRGB);
      if (hTarget) cmsCloseProfile(hTarget);
    }

    if (settings->format == OFL_EXPORT_FORMAT_JPEG) {
      success = write_jpeg_file(dest_path, rgb8, out_w, out_h,
                                settings->jpeg_quality,
                                settings->embed_profile,
                                icc_data, icc_len,
                                error);
    } else {
      success = write_tiff_file(dest_path, rgb8, out_w, out_h,
                                FALSE,
                                settings->embed_profile,
                                icc_data, icc_len,
                                error);
    }
    g_free(rgb8);
  } else {
    // 16-bit TIFF
    int out_w = 0, out_h = 0;
    uint16_t *rgb16 = ofl_neg_convert_to_rgb16(img->rgb, img->width, img->height, &p, &out_w, &out_h);
    ofl_rgb16_free(img);

    // Apply Crop if enabled
    if (crop && crop->enabled && crop->width > 0.001f && crop->height > 0.001f) {
      int cx = (int)roundf(crop->x * (float)out_w);
      int cy = (int)roundf(crop->y * (float)out_h);
      int cw = (int)roundf(crop->width * (float)out_w);
      int ch = (int)roundf(crop->height * (float)out_h);
      if (cx < 0) cx = 0;
      if (cy < 0) cy = 0;
      if (cx + cw > out_w) cw = out_w - cx;
      if (cy + ch > out_h) ch = out_h - cy;
      if (cw > 0 && ch > 0 && (cw != out_w || ch != out_h)) {
        uint16_t *cropped = g_malloc((size_t)cw * (size_t)ch * 3 * sizeof(uint16_t));
        for (int y = 0; y < ch; y++) {
          memcpy(&cropped[y * cw * 3], &rgb16[((cy + y) * out_w + cx) * 3], (size_t)cw * 3 * sizeof(uint16_t));
        }
        g_free(rgb16);
        rgb16 = cropped;
        out_w = cw;
        out_h = ch;
      }
    }

    if (settings->profile != OFL_PROFILE_SRGB) {
      cmsHPROFILE hsRGB = cmsCreate_sRGBProfile();
      cmsHPROFILE hTarget = create_cms_profile(settings->profile);
      if (hsRGB && hTarget) {
        cmsHTRANSFORM xform = cmsCreateTransform(hsRGB, TYPE_RGB_16, hTarget, TYPE_RGB_16, INTENT_PERCEPTUAL, 0);
        if (xform) {
          cmsDoTransform(xform, rgb16, rgb16, (cmsUInt32Number)((size_t)out_w * (size_t)out_h));
          cmsDeleteTransform(xform);
        }
      }
      if (hsRGB) cmsCloseProfile(hsRGB);
      if (hTarget) cmsCloseProfile(hTarget);
    }

    success = write_tiff_file(dest_path, rgb16, out_w, out_h,
                              TRUE,
                              settings->embed_profile,
                              icc_data, icc_len,
                              error);
    g_free(rgb16);
  }

  g_free(icc_data);

  if (success && out_saved_path) {
    *out_saved_path = dest_path;
  } else if (!success) {
    g_free(dest_path);
  }

  return success;
}
