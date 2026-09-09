#include "ofl_app.h"
#include "ofl_rawthumb.h"
#include "ofl_rawdecode.h"
#include "ofl_tiffdecode.h"
#include "ofl_negpipe.h"
#include "ofl_frame.h"
#include "ofl_export.h"
#include "ofl_app_icon.h"

#include <gtk/gtk.h>

#define OFL_DEFAULT_BASE_R 0.7909f
#define OFL_DEFAULT_BASE_G 0.5124f
#define OFL_DEFAULT_BASE_B 0.3556f
#define OFL_DEFAULT_BASE_8BIT_R 247
#define OFL_DEFAULT_BASE_8BIT_G 203
#define OFL_DEFAULT_BASE_8BIT_B 172

typedef struct {
  GtkApplication *app;
  GtkWindow      *win;

  GtkPicture     *preview;

  GListStore        *frames;   // list of OflFrame*
  GtkMultiSelection *sel;
  guint             active_frame_idx;
  GFile             *current_roll_folder;

  GtkGridView    *filmstrip_grid;

  // Negative pipeline defaults (gamma, etc.)
  OflNegParams neg;

  // Last decoded RGB16 image for current selection (used for base picking)
  OflRgb16 *last_img;
  char     *last_img_path;

  // Roll-level base pick workflow
  gboolean pick_base_mode;
  gboolean has_picked_base;
  gboolean has_roll_base;
  float roll_base_rgb[3];
  float hover_rgb[3];
  float display_rgb[3];
  uint16_t preview_white_pt;

  // UI elements for live base feedback & actions
  GtkButton *btn_pick;
  GtkButton *btn_apply;
  GtkButton *btn_export;
  GtkWidget *hud_box;
  GtkWidget *hud_swatch;
  GtkWidget *hud_label;

  // Roll background thumbnail task management
  gint64 roll_epoch;

  // Individual image adjustment panel UI
  GtkWidget *adj_panel;
  GtkScale  *scale_temp;
  GtkScale  *scale_tint;
  GtkScale  *scale_exposure;
  GtkScale  *scale_contrast;
  GtkScale  *scale_brightness;

  GtkEntry  *entry_val_temp;
  GtkEntry  *entry_val_tint;
  GtkEntry  *entry_val_exposure;
  GtkEntry  *entry_val_contrast;
  GtkEntry  *entry_val_brightness;

  gboolean   is_updating_adj_ui;

  // Crop mode state & UI
  GtkButton     *btn_crop;
  gboolean       crop_mode;
  GtkWidget     *crop_overlay;
  int            crop_hit;
  double         crop_drag_start_x;
  double         crop_drag_start_y;
  OflCrop        crop_start_rect;
} OflApp;

typedef enum {
  CROP_HIT_NONE = 0,
  CROP_HIT_INSIDE,
  CROP_HIT_TL,
  CROP_HIT_TR,
  CROP_HIT_BL,
  CROP_HIT_BR,
  CROP_HIT_TOP,
  CROP_HIT_BOTTOM,
  CROP_HIT_LEFT,
  CROP_HIT_RIGHT
} CropHitTarget;

static void render_selected_preview(OflApp *s);
static void on_crop_tool_clicked(GtkButton *button, gpointer user_data);
static void on_reset_crop_clicked(GtkButton *button, gpointer user_data);
static void update_crop_cursor(OflApp *s, int hit);
static void draw_crop_overlay_cb(GtkDrawingArea *drawing_area, cairo_t *cr, int width, int height, gpointer user_data);
static void on_crop_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);
static void on_crop_motion_leave(GtkEventControllerMotion *controller, gpointer user_data);
static void on_crop_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data);
static void on_crop_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);
static void on_crop_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data);

// Ensure buffer is RGBA8 (stride = w*4), converting from RGB8 or other layouts if needed.
static guint8 *ensure_rgba8(guint8 *buf, int w, int h, int *io_stride) {
  if (!buf || w <= 0 || h <= 0 || !io_stride) return buf;

  const int stride = *io_stride;
  const int want = w * 4;

  // Already RGBA8
  if (stride == want) return buf;

  // Common case: RGB8
  if (stride == w * 3) {
    guint8 *out = g_malloc0((gsize)want * (gsize)h);
    for (int y = 0; y < h; y++) {
      const guint8 *src = buf + (gsize)y * (gsize)stride;
      guint8 *dst = out + (gsize)y * (gsize)want;
      for (int x = 0; x < w; x++) {
        dst[x*4 + 0] = src[x*3 + 0];
        dst[x*4 + 1] = src[x*3 + 1];
        dst[x*4 + 2] = src[x*3 + 2];
        dst[x*4 + 3] = 255;
      }
    }
    g_free(buf);
    *io_stride = want;
    return out;
  }

  // Any other unexpected stride: re-pack defensively using minimum bytes per pixel we can infer.
  // If stride is smaller than RGBA expectation, force-copy only what fits and set alpha.
  guint8 *out = g_malloc0((gsize)want * (gsize)h);
  int bpp = stride / w;
  if (bpp < 3) bpp = 3;
  for (int y = 0; y < h; y++) {
    const guint8 *src = buf + (gsize)y * (gsize)stride;
    guint8 *dst = out + (gsize)y * (gsize)want;
    for (int x = 0; x < w; x++) {
      const guint8 *p = src + x * bpp;
      dst[x*4 + 0] = p[0];
      dst[x*4 + 1] = p[1];
      dst[x*4 + 2] = p[2];
      dst[x*4 + 3] = 255;
    }
  }
  g_free(buf);
  *io_stride = want;
  return out;
}

// ---------- Helpers ----------
static gboolean is_supported_image_ext(const char *name) {
  const char *ext = strrchr(name, '.');
  if (!ext) return FALSE;
  ext++;

  const char *exts[] = {"arw","nef","nrw","raf","cr2","cr3","dng","tif","tiff", NULL};
  for (int i = 0; exts[i]; i++) {
    if (g_ascii_strcasecmp(ext, exts[i]) == 0) return TRUE;
  }
  return FALSE;
}

static void reset_adjustments_ui(OflApp *s);

static void clear_frames(OflApp *s) {
  s->roll_epoch++;
  g_list_store_remove_all(s->frames);
  if (s->last_img) {
    ofl_rgb16_free(s->last_img);
    s->last_img = NULL;
  }
  g_clear_pointer(&s->last_img_path, g_free);
  s->has_picked_base = FALSE;
  s->has_roll_base = FALSE;
  s->pick_base_mode = FALSE;
  s->roll_base_rgb[0] = OFL_DEFAULT_BASE_R;
  s->roll_base_rgb[1] = OFL_DEFAULT_BASE_G;
  s->roll_base_rgb[2] = OFL_DEFAULT_BASE_B;
  s->display_rgb[0] = (float)OFL_DEFAULT_BASE_8BIT_R / 255.0f;
  s->display_rgb[1] = (float)OFL_DEFAULT_BASE_8BIT_G / 255.0f;
  s->display_rgb[2] = (float)OFL_DEFAULT_BASE_8BIT_B / 255.0f;
  reset_adjustments_ui(s);
  s->active_frame_idx = 0;
  if (s->current_roll_folder) {
    g_object_unref(s->current_roll_folder);
    s->current_roll_folder = NULL;
  }
  if (s->btn_export) {
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_export), FALSE);
  }
  if (s->hud_box) gtk_widget_set_visible(s->hud_box, FALSE);
  if (s->btn_apply) gtk_widget_remove_css_class(GTK_WIDGET(s->btn_apply), "suggested-action");
  if (s->btn_pick) {
    gtk_button_set_label(s->btn_pick, "Pick Film Base");
    gtk_widget_remove_css_class(GTK_WIDGET(s->btn_pick), "suggested-action");
  }
  s->crop_mode = FALSE;
  if (s->btn_crop) {
    gtk_widget_remove_css_class(GTK_WIDGET(s->btn_crop), "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_crop), FALSE);
  }
  if (s->crop_overlay) gtk_widget_set_visible(s->crop_overlay, FALSE);
}

static void scan_folder(OflApp *s, GFile *folder) {
  clear_frames(s);
  s->current_roll_folder = g_object_ref(folder);

  GError *err = NULL;
  GFileEnumerator *en = g_file_enumerate_children(
    folder,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
    G_FILE_QUERY_INFO_NONE,
    NULL,
    &err
  );

  if (!en) {
    g_warning("enumerate failed: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    return;
  }

  for (;;) {
    GFileInfo *info = g_file_enumerator_next_file(en, NULL, &err);
    if (!info) break;

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) {
      const char *name = g_file_info_get_name(info);
      // Skip hidden files, system files, and macOS AppleDouble files (._*)
      if (name && name[0] != '.' && is_supported_image_ext(name)) {
        GFile *child = g_file_get_child(folder, name);

        OflFrame *fr = ofl_frame_new(child);
        g_list_store_append(s->frames, fr);

        g_object_unref(fr);
        g_object_unref(child);
      }
    }

    g_object_unref(info);
  }

  if (err) {
    g_warning("enumeration error: %s", err->message);
    g_clear_error(&err);
  }

  g_object_unref(en);

  // Force filmstrip to a single row: columns = item count (prevents wrapping)
  if (s->filmstrip_grid) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(s->frames));
    guint cols = (n > 0) ? n : 1;
    gtk_grid_view_set_min_columns(s->filmstrip_grid, cols);
    gtk_grid_view_set_max_columns(s->filmstrip_grid, cols);
  }
}

// Map click coords (widget space) -> image pixel coords, accounting for fit-to-screen letterboxing.
static gboolean map_widget_to_image(GtkWidget *w, int img_w, int img_h,
                                    double wx, double wy, int *ix, int *iy) {
  int ww = gtk_widget_get_width(w);
  int wh = gtk_widget_get_height(w);
  if (ww <= 0 || wh <= 0) return FALSE;

  double img_ar = (double)img_w / (double)img_h;
  double win_ar = (double)ww / (double)wh;

  double draw_w, draw_h, off_x, off_y;
  if (win_ar > img_ar) {
    // window wider → height fits
    draw_h = (double)wh;
    draw_w = draw_h * img_ar;
    off_x = ((double)ww - draw_w) * 0.5;
    off_y = 0.0;
  } else {
    // window taller → width fits
    draw_w = (double)ww;
    draw_h = draw_w / img_ar;
    off_x = 0.0;
    off_y = ((double)wh - draw_h) * 0.5;
  }

  if (wx < off_x || wx >= off_x + draw_w || wy < off_y || wy >= off_y + draw_h)
    return FALSE;

  double nx = (wx - off_x) / draw_w;
  double ny = (wy - off_y) / draw_h;

  int x = (int)(nx * img_w);
  int y = (int)(ny * img_h);
  if (x < 0) x = 0;
  if (x >= img_w) x = img_w - 1;
  if (y < 0) y = 0;
  if (y >= img_h) y = img_h - 1;

  *ix = x; *iy = y;
  return TRUE;
}

static void sample_base_at(OflRgb16 *img, int cx, int cy, float out_base[3]) {
  const int r = 5; // 11x11
  double sum[3] = {0,0,0};
  double cnt = 0;

  for (int dy=-r; dy<=r; dy++) {
    int y = cy + dy;
    if (y < 0 || y >= img->height) continue;
    for (int dx=-r; dx<=r; dx++) {
      int x = cx + dx;
      if (x < 0 || x >= img->width) continue;
      const uint16_t *px = &img->rgb[(y*img->width + x)*3];
      sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
      cnt++;
    }
  }

  out_base[0] = (float)((sum[0]/cnt) / 65535.0);
  out_base[1] = (float)((sum[1]/cnt) / 65535.0);
  out_base[2] = (float)((sum[2]/cnt) / 65535.0);

  for (int c=0;c<3;c++) {
    if (out_base[c] < 0.0001f) out_base[c] = 0.0001f;
    if (out_base[c] > 1.0f)    out_base[c] = 1.0f;
  }
}

// Convert 16-bit linear RAW negative to 8-bit RGBA for display with normalized exposure & sRGB gamma
static guint8* ofl_raw_preview_to_rgba8(const uint16_t *rgb, int w, int h, int rot,
                                       const OflAdjustments *adj,
                                       uint16_t *out_white_pt,
                                       int *out_w, int *out_h, int *out_stride) {
  if (!rgb || w <= 0 || h <= 0) return NULL;

  size_t npx = (size_t)w * (size_t)h;

  // Subsample to quickly find the 99.9th percentile white point across all channels (< 1ms)
  size_t step = (npx > 500000) ? 8 : 1;
  uint32_t hist[65536] = {0};
  size_t counted = 0;
  for (size_t i = 0; i < npx; i += step) {
    const uint16_t *px = &rgb[i * 3];
    uint16_t m = px[0];
    if (px[1] > m) m = px[1];
    if (px[2] > m) m = px[2];
    hist[m]++;
    counted++;
  }

  uint64_t target = (uint64_t)(0.999 * (double)counted);
  uint64_t acc = 0;
  uint16_t white_pt = 65535;
  for (int v = 0; v < 65536; v++) {
    acc += hist[v];
    if (acc >= target) {
      white_pt = (uint16_t)v;
      break;
    }
  }
  if (white_pt < 1000) white_pt = 65535;
  if (out_white_pt) *out_white_pt = white_pt;

  float temp_norm = adj ? CLAMP(adj->temperature / 100.0f, -1.0f, 1.0f) : 0.0f;
  float tint_norm = adj ? CLAMP(adj->tint / 100.0f, -1.0f, 1.0f) : 0.0f;
  float exp_ev    = adj ? CLAMP(adj->exposure, -2.0f, 2.0f) : 0.0f;
  float c_norm    = adj ? CLAMP(adj->contrast / 100.0f, -1.0f, 1.0f) : 0.0f;
  float b_offset  = adj ? CLAMP(adj->brightness / 100.0f, -1.0f, 1.0f) * 0.5f : 0.0f;

  float exp_mult = powf(2.0f, exp_ev);
  float wb_gain[3];
  wb_gain[0] = powf(2.0f,  temp_norm * 0.5f);
  wb_gain[1] = powf(2.0f, -tint_norm * 0.5f);
  wb_gain[2] = powf(2.0f, -temp_norm * 0.5f);

  float c_factor = powf(2.0f, c_norm);

  // Build sRGB gamma (1/2.2) + adjustments 16-bit to 8-bit lookup table per channel
  uint8_t lut[3][65536];
  const float inv_white = 1.0f / (float)white_pt;
  const float inv_gamma = 1.0f / 2.2f;

  for (int c = 0; c < 3; c++) {
    float channel_mult = wb_gain[c] * exp_mult;
    for (int v = 0; v < 65536; v++) {
      float norm = (float)v * inv_white;
      float lin = CLAMP(norm * channel_mult, 0.0f, 1.0f);
      float v_p = powf(lin, inv_gamma);
      v_p = (v_p - 0.5f) * c_factor + 0.5f;
      v_p = v_p + b_offset;
      v_p = CLAMP(v_p, 0.0f, 1.0f);
      lut[c][v] = (uint8_t)(v_p * 255.0f + 0.5f);
    }
  }

  // Rotation output dimensions
  rot = ((rot % 4) + 4) % 4;
  int W = (rot % 2 == 0) ? w : h;
  int H = (rot % 2 == 0) ? h : w;

  int stride = W * 4;
  guint8 *out = g_malloc((size_t)stride * (size_t)H);

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;
  if (out_stride) *out_stride = stride;

  if (rot == 0) {
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

// Map widget click/hover coordinates back to unrotated pixel coordinates in s->last_img
static gboolean get_current_image_pixel(OflApp *s, double wx, double wy, int *out_ix, int *out_iy) {
  if (!s->last_img) return FALSE;

  int w = s->last_img->width;
  int h = s->last_img->height;

  guint idx = s->active_frame_idx;
  int rot = 0;
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (idx < n_frames) {
    OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
    if (fr) {
      rot = ofl_frame_get_rotate_steps(fr);
      g_object_unref(fr);
    }
  }

  rot = ((rot % 4) + 4) % 4;
  int disp_w = (rot % 2 == 0) ? w : h;
  int disp_h = (rot % 2 == 0) ? h : w;

  int disp_x = 0, disp_y = 0;
  if (!map_widget_to_image(GTK_WIDGET(s->preview), disp_w, disp_h, wx, wy, &disp_x, &disp_y)) {
    return FALSE;
  }

  int ix = 0, iy = 0;
  switch (rot) {
    case 0: ix = disp_x;         iy = disp_y;         break;
    case 1: ix = disp_y;         iy = h - 1 - disp_x; break;
    case 2: ix = w - 1 - disp_x; iy = h - 1 - disp_y; break;
    case 3: ix = w - 1 - disp_y; iy = disp_x;         break;
  }

  if (ix < 0) ix = 0; if (ix >= w) ix = w - 1;
  if (iy < 0) iy = 0; if (iy >= h) iy = h - 1;

  *out_ix = ix;
  *out_iy = iy;
  return TRUE;
}

// ---------- Thumbnail & Background Conversion Helpers ----------
static guint8* create_thumb_pixels_rgba8(const guint8 *src, int sw, int sh, int s_stride, int max_dim,
                                         int *out_tw, int *out_th, int *out_t_stride) {
  if (!src || sw <= 0 || sh <= 0 || max_dim <= 0) return NULL;

  int tw, th;
  if (sw >= sh) {
    tw = max_dim;
    th = (int)((double)sh * max_dim / sw);
    if (th < 1) th = 1;
  } else {
    th = max_dim;
    tw = (int)((double)sw * max_dim / sh);
    if (tw < 1) tw = 1;
  }

  int t_stride = tw * 4;
  guint8 *thumb_buf = g_malloc((size_t)t_stride * (size_t)th);

  double scale_x = (double)sw / (double)tw;
  double scale_y = (double)sh / (double)th;

  for (int ty = 0; ty < th; ty++) {
    int sy = (int)(ty * scale_y);
    if (sy >= sh) sy = sh - 1;
    const guint8 *s_row = src + (size_t)sy * (size_t)s_stride;
    guint8 *d_row = thumb_buf + (size_t)ty * (size_t)t_stride;

    for (int tx = 0; tx < tw; tx++) {
      int sx = (int)(tx * scale_x);
      if (sx >= sw) sx = sw - 1;
      const guint8 *sp = s_row + sx * 4;
      guint8 *dp = d_row + tx * 4;
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
      dp[3] = 255;
    }
  }

  if (out_tw) *out_tw = tw;
  if (out_th) *out_th = th;
  if (out_t_stride) *out_t_stride = t_stride;
  return thumb_buf;
}

typedef struct {
  OflApp     *app;
  gint64      roll_epoch;
  OflFrame   *frame;
  guint8     *thumb_buf;
  int         tw;
  int         th;
  int         t_stride;
  guint       idx;
} ThumbResult;

static gboolean on_thumb_ready_idle(gpointer user_data) {
  ThumbResult *res = user_data;
  if (res->app->roll_epoch == res->roll_epoch && res->thumb_buf) {
    GBytes *bytes = g_bytes_new_take(res->thumb_buf, (size_t)res->t_stride * (size_t)res->th);
    GdkTexture *tex = gdk_memory_texture_new(res->tw, res->th, GDK_MEMORY_R8G8B8A8, bytes, (gsize)res->t_stride);
    g_bytes_unref(bytes);

    if (tex) {
      ofl_frame_set_thumbnail(res->frame, tex);
      g_object_unref(tex);
    }
  } else {
    g_free(res->thumb_buf);
  }

  g_object_unref(res->frame);
  g_free(res);
  return G_SOURCE_REMOVE;
}

static OflRgb16* ofl_load_image_rgb16(const char *path, GError **error) {
  if (ofl_is_tiff_path(path)) {
    return ofl_tiffdecode_rgb16(path, error);
  }
  return ofl_rawdecode_rgb16_preview(path, error);
}

static GdkTexture* ofl_load_initial_thumbnail(const char *path, GError **error) {
  if (ofl_is_tiff_path(path)) {
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 160, 160, TRUE, &err);
    if (!pb) {
      if (error && err) *error = err;
      else if (err) g_clear_error(&err);
      return NULL;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rowstride = gdk_pixbuf_get_rowstride(pb);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pb);
    GBytes *bytes = g_bytes_new(gdk_pixbuf_read_pixels(pb), (size_t)rowstride * (size_t)h);
    GdkTexture *tex = gdk_memory_texture_new(w, h,
                                             has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                             bytes,
                                             (gsize)rowstride);
    g_bytes_unref(bytes);
    g_object_unref(pb);
    return tex;
  }

  // Camera RAW: read embedded JPEG thumbnail
  return ofl_rawthumb_texture_from_file(path, error);
}

typedef struct {
  char     *path;
  OflFrame *frame;
  int       rot;
  guint     idx;
} RollItemSnap;

typedef struct {
  OflApp       *app;
  RollItemSnap *items;
  guint         n_items;
  OflNegParams  params;
  guint         cur_idx;
  gint64        roll_epoch;
} RollThumbTask;

static gpointer roll_thumb_worker(gpointer user_data) {
  RollThumbTask *task = user_data;

  for (guint i = 0; i < task->n_items; i++) {
    if (task->app->roll_epoch != task->roll_epoch) break;
    if (i == task->cur_idx) continue;
    if (!task->items[i].path || !task->items[i].frame) continue;

    OflNegParams p = task->params;
    p.rotate_steps = task->items[i].rot;
    const OflAdjustments *adj = ofl_frame_get_adjustments(task->items[i].frame);
    if (adj) {
      p.temperature = adj->temperature;
      p.tint = adj->tint;
      p.exposure = adj->exposure;
      p.contrast = adj->contrast;
      p.brightness = adj->brightness;
    }

    GError *err = NULL;
    OflRgb16 *img = ofl_load_image_rgb16(task->items[i].path, &err);
    if (!img) {
      if (err) g_clear_error(&err);
      continue;
    }

    if (task->app->roll_epoch != task->roll_epoch) {
      ofl_rgb16_free(img);
      break;
    }

    int out_w = 0, out_h = 0, stride = 0;
    guint8 *rgba = ofl_neg_convert_to_rgba8(img->rgb, img->width, img->height,
                                           &p, &out_w, &out_h, &stride);
    ofl_rgb16_free(img);

    if (rgba && out_w > 0 && out_h > 0) {
      int tw, th, t_stride;
      guint8 *thumb_buf = create_thumb_pixels_rgba8(rgba, out_w, out_h, stride, 160,
                                                    &tw, &th, &t_stride);
      g_free(rgba);

      if (thumb_buf) {
        ThumbResult *res = g_new0(ThumbResult, 1);
        res->app = task->app;
        res->roll_epoch = task->roll_epoch;
        res->frame = g_object_ref(task->items[i].frame);
        res->thumb_buf = thumb_buf;
        res->tw = tw;
        res->th = th;
        res->t_stride = t_stride;
        res->idx = task->items[i].idx;

        g_idle_add(on_thumb_ready_idle, res);
      }
    } else {
      g_free(rgba);
    }
  }

  for (guint i = 0; i < task->n_items; i++) {
    g_free(task->items[i].path);
    if (task->items[i].frame) g_object_unref(task->items[i].frame);
  }
  g_free(task->items);
  g_free(task);
  return NULL;
}

static void start_roll_thumbnails_conversion(OflApp *s) {
  s->roll_epoch++;

  guint cur_idx = s->active_frame_idx;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(s->frames));
  if (n <= 1) return;

  RollThumbTask *task = g_new0(RollThumbTask, 1);
  task->app = s;
  task->params = s->neg;
  task->params.base_rgb[0] = s->roll_base_rgb[0];
  task->params.base_rgb[1] = s->roll_base_rgb[1];
  task->params.base_rgb[2] = s->roll_base_rgb[2];
  task->cur_idx = cur_idx;
  task->roll_epoch = s->roll_epoch;
  task->items = g_new0(RollItemSnap, n);
  task->n_items = n;

  for (guint i = 0; i < n; i++) {
    OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), i);
    if (!fr) continue;
    GFile *f = ofl_frame_get_file(fr);
    task->items[i].path = f ? g_file_get_path(f) : NULL;
    task->items[i].frame = g_object_ref(fr);
    task->items[i].rot = ofl_frame_get_rotate_steps(fr);
    task->items[i].idx = i;
    g_object_unref(fr);
  }

  GThread *th = g_thread_new("roll_thumb_worker", roll_thumb_worker, task);
  g_thread_unref(th);
}

// Swatch drawing callback for live RGB HUD
static void draw_swatch_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
  (void)area;
  OflApp *s = user_data;
  double cx = width * 0.5;
  double cy = height * 0.5;
  double r = (width < height ? width : height) * 0.5 - 1.5;
  if (r < 2.0) r = 2.0;

  cairo_arc(cr, cx, cy, r, 0, 2.0 * G_PI);
  cairo_set_source_rgb(cr, s->display_rgb[0], s->display_rgb[1], s->display_rgb[2]);
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);
}

// Mouse motion callback for live RGB sampling
static void on_preview_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data) {
  (void)ctrl;
  OflApp *s = user_data;
  if (!s->pick_base_mode || !s->last_img) return;

  int ix, iy;
  if (!get_current_image_pixel(s, x, y, &ix, &iy)) {
    return;
  }

  sample_base_at(s->last_img, ix, iy, s->hover_rgb);

  // Compute display RGB matching the visual preview on screen
  float w_pt = (s->preview_white_pt > 1000) ? (float)s->preview_white_pt : 65535.0f;
  const float inv_gamma = 1.0f / 2.2f;
  s->display_rgb[0] = powf(CLAMP(s->hover_rgb[0] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  s->display_rgb[1] = powf(CLAMP(s->hover_rgb[1] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  s->display_rgb[2] = powf(CLAMP(s->hover_rgb[2] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  gtk_widget_queue_draw(s->hud_swatch);

  int r = (int)(CLAMP(s->display_rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
  int g = (int)(CLAMP(s->display_rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
  int b = (int)(CLAMP(s->display_rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);

  char buf[128];
  snprintf(buf, sizeof(buf), "R: %d   G: %d   B: %d   (Click to sample)", r, g, b);
  gtk_label_set_text(GTK_LABEL(s->hud_label), buf);
  gtk_widget_set_visible(s->hud_box, TRUE);
}
static void update_adj_readout_labels(OflApp *s, const OflAdjustments *adj) {
  if (!s->entry_val_temp || !adj) return;
  char buf[32];

  // Temp (-100 .. +100)
  if (s->is_updating_adj_ui || !gtk_widget_has_focus(GTK_WIDGET(s->entry_val_temp))) {
    int t = (int)roundf(adj->temperature);
    if (t == 0) snprintf(buf, sizeof(buf), "0");
    else snprintf(buf, sizeof(buf), "%+d", t);
    gtk_editable_set_text(GTK_EDITABLE(s->entry_val_temp), buf);
  }

  // Tint (-100 .. +100)
  if (s->is_updating_adj_ui || !gtk_widget_has_focus(GTK_WIDGET(s->entry_val_tint))) {
    int ti = (int)roundf(adj->tint);
    if (ti == 0) snprintf(buf, sizeof(buf), "0");
    else snprintf(buf, sizeof(buf), "%+d", ti);
    gtk_editable_set_text(GTK_EDITABLE(s->entry_val_tint), buf);
  }

  // Exposure (+- 2 stops EV)
  if (s->is_updating_adj_ui || !gtk_widget_has_focus(GTK_WIDGET(s->entry_val_exposure))) {
    if (fabsf(adj->exposure) < 0.005f) {
      snprintf(buf, sizeof(buf), "0.00");
    } else {
      snprintf(buf, sizeof(buf), "%+.2f", adj->exposure);
    }
    gtk_editable_set_text(GTK_EDITABLE(s->entry_val_exposure), buf);
  }

  // Contrast (-100 .. +100)
  if (s->is_updating_adj_ui || !gtk_widget_has_focus(GTK_WIDGET(s->entry_val_contrast))) {
    int c = (int)roundf(adj->contrast);
    if (c == 0) snprintf(buf, sizeof(buf), "0");
    else snprintf(buf, sizeof(buf), "%+d", c);
    gtk_editable_set_text(GTK_EDITABLE(s->entry_val_contrast), buf);
  }

  // Brightness (-100 .. +100)
  if (s->is_updating_adj_ui || !gtk_widget_has_focus(GTK_WIDGET(s->entry_val_brightness))) {
    int b = (int)roundf(adj->brightness);
    if (b == 0) snprintf(buf, sizeof(buf), "0");
    else snprintf(buf, sizeof(buf), "%+d", b);
    gtk_editable_set_text(GTK_EDITABLE(s->entry_val_brightness), buf);
  }
}

static void reset_adjustments_ui(OflApp *s) {
  if (!s->adj_panel) return;
  s->is_updating_adj_ui = TRUE;
  if (s->scale_temp) gtk_range_set_value(GTK_RANGE(s->scale_temp), 0.0);
  if (s->scale_tint) gtk_range_set_value(GTK_RANGE(s->scale_tint), 0.0);
  if (s->scale_exposure) gtk_range_set_value(GTK_RANGE(s->scale_exposure), 0.0);
  if (s->scale_contrast) gtk_range_set_value(GTK_RANGE(s->scale_contrast), 0.0);
  if (s->scale_brightness) gtk_range_set_value(GTK_RANGE(s->scale_brightness), 0.0);

  OflAdjustments def;
  ofl_adjustments_init_defaults(&def);
  update_adj_readout_labels(s, &def);
  s->is_updating_adj_ui = FALSE;
  gtk_widget_set_sensitive(s->adj_panel, FALSE);
}

static void sync_adjustments_ui_from_selected_frame(OflApp *s) {
  if (!s->adj_panel) return;
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) {
    reset_adjustments_ui(s);
    return;
  }
  guint idx = s->active_frame_idx;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!fr) {
    reset_adjustments_ui(s);
    return;
  }

  gtk_widget_set_sensitive(s->adj_panel, TRUE);
  const OflAdjustments *adj = ofl_frame_get_adjustments(fr);

  s->is_updating_adj_ui = TRUE;
  if (s->scale_temp) gtk_range_set_value(GTK_RANGE(s->scale_temp), adj->temperature);
  if (s->scale_tint) gtk_range_set_value(GTK_RANGE(s->scale_tint), adj->tint);
  if (s->scale_exposure) gtk_range_set_value(GTK_RANGE(s->scale_exposure), adj->exposure);
  if (s->scale_contrast) gtk_range_set_value(GTK_RANGE(s->scale_contrast), adj->contrast);
  if (s->scale_brightness) gtk_range_set_value(GTK_RANGE(s->scale_brightness), adj->brightness);

  update_adj_readout_labels(s, adj);
  s->is_updating_adj_ui = FALSE;

  g_object_unref(fr);
}

typedef struct {
  OflApp    *app;
  GtkScale  *scale;
  GtkEntry  *entry;
  double     min_val;
  double     max_val;
  double     step;
  gboolean   is_exposure;
  gint64     last_double_click_time;
} AdjSliderData;

static void on_adj_slider_changed(GtkRange *range, gpointer user_data) {
  OflApp *s = user_data;
  if (s->is_updating_adj_ui) return;

  if (range) {
    AdjSliderData *d = (AdjSliderData*)g_object_get_data(G_OBJECT(range), "slider_data");
    if (d && d->last_double_click_time > 0) {
      gint64 now = g_get_monotonic_time();
      if (now - d->last_double_click_time < 350000) { // 350ms window
        double v = gtk_range_get_value(range);
        if (fabs(v) > 1e-5) {
          gtk_range_set_value(range, 0.0);
          return;
        }
      }
    }
  }

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  guint idx = s->active_frame_idx;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!fr) return;

  OflAdjustments adj;
  adj.temperature = (float)gtk_range_get_value(GTK_RANGE(s->scale_temp));
  adj.tint        = (float)gtk_range_get_value(GTK_RANGE(s->scale_tint));
  adj.exposure    = (float)gtk_range_get_value(GTK_RANGE(s->scale_exposure));
  adj.contrast    = (float)gtk_range_get_value(GTK_RANGE(s->scale_contrast));
  adj.brightness  = (float)gtk_range_get_value(GTK_RANGE(s->scale_brightness));

  update_adj_readout_labels(s, &adj);
  ofl_frame_set_adjustments(fr, &adj);
  g_object_unref(fr);

  render_selected_preview(s);
}

static void on_reset_temp_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  if (s->scale_temp) gtk_range_set_value(GTK_RANGE(s->scale_temp), 0.0);
}

static void on_reset_tint_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  if (s->scale_tint) gtk_range_set_value(GTK_RANGE(s->scale_tint), 0.0);
}

static void on_reset_exposure_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  if (s->scale_exposure) gtk_range_set_value(GTK_RANGE(s->scale_exposure), 0.0);
}

static void on_reset_contrast_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  if (s->scale_contrast) gtk_range_set_value(GTK_RANGE(s->scale_contrast), 0.0);
}

static void on_reset_brightness_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  if (s->scale_brightness) gtk_range_set_value(GTK_RANGE(s->scale_brightness), 0.0);
}

static void on_reset_all_adjustments_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  OflApp *s = user_data;
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  guint idx = s->active_frame_idx;

  s->is_updating_adj_ui = TRUE;
  if (s->scale_temp) gtk_range_set_value(GTK_RANGE(s->scale_temp), 0.0);
  if (s->scale_tint) gtk_range_set_value(GTK_RANGE(s->scale_tint), 0.0);
  if (s->scale_exposure) gtk_range_set_value(GTK_RANGE(s->scale_exposure), 0.0);
  if (s->scale_contrast) gtk_range_set_value(GTK_RANGE(s->scale_contrast), 0.0);
  if (s->scale_brightness) gtk_range_set_value(GTK_RANGE(s->scale_brightness), 0.0);

  OflAdjustments def;
  ofl_adjustments_init_defaults(&def);
  update_adj_readout_labels(s, &def);
  s->is_updating_adj_ui = FALSE;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (fr) {
    ofl_frame_set_adjustments(fr, &def);
    g_object_unref(fr);
    render_selected_preview(s);
  }
}


static gboolean parse_and_validate_adj_value(const char *text, double min_val, double max_val, double *out_val) {
  if (!text || !out_val) return FALSE;

  while (*text && g_ascii_isspace(*text)) text++;
  if (*text == '\0') return FALSE;

  char buf[64];
  g_strlcpy(buf, text, sizeof(buf));
  g_strstrip(buf);

  // Strip trailing unit strings: "stops", "stop", "ev", "v", "%"
  size_t len = strlen(buf);
  if (len >= 5 && g_ascii_strcasecmp(&buf[len - 5], "stops") == 0) {
    buf[len - 5] = '\0';
    g_strstrip(buf);
  } else if (len >= 4 && g_ascii_strcasecmp(&buf[len - 4], "stop") == 0) {
    buf[len - 4] = '\0';
    g_strstrip(buf);
  } else if (len >= 2 && g_ascii_strcasecmp(&buf[len - 2], "ev") == 0) {
    buf[len - 2] = '\0';
    g_strstrip(buf);
  } else if (len >= 1 && (buf[len - 1] == '%' || buf[len - 1] == 'v' || buf[len - 1] == 'V')) {
    buf[len - 1] = '\0';
    g_strstrip(buf);
  }

  if (buf[0] == '\0') return FALSE;

  char *endptr = NULL;
  double val = g_ascii_strtod(buf, &endptr);
  if (endptr == buf) {
    return FALSE;
  }

  while (*endptr && g_ascii_isspace(*endptr)) endptr++;
  if (*endptr != '\0') {
    return FALSE;
  }

  if (isnan(val) || isinf(val)) {
    return FALSE;
  }

  if (fabs(val) < 1e-6) val = 0.0;

  if (val < min_val) val = min_val;
  if (val > max_val) val = max_val;

  *out_val = val;
  return TRUE;
}

static void disable_entry_text_drag(GtkEntry *entry) {
  if (!entry) return;
  GtkEditable *delegate = gtk_editable_get_delegate(GTK_EDITABLE(entry));
  if (!delegate) return;
  GtkWidget *text_widget = GTK_WIDGET(delegate);

  GListModel *controllers = gtk_widget_observe_controllers(text_widget);
  guint n = g_list_model_get_n_items(controllers);
  for (guint i = 0; i < n; i++) {
    gpointer c = g_list_model_get_item(controllers, i);
    if (GTK_IS_GESTURE_DRAG(c)) {
      gtk_widget_remove_controller(text_widget, GTK_EVENT_CONTROLLER(c));
      g_object_unref(c);
      break;
    }
    g_object_unref(c);
  }
  g_object_unref(controllers);
}

static void commit_adj_entry_value(AdjSliderData *d) {
  if (!d || !d->app || !d->scale || !d->entry) return;
  OflApp *s = d->app;

  const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry));
  double val = 0.0;

  if (parse_and_validate_adj_value(text, d->min_val, d->max_val, &val)) {
    gtk_widget_remove_css_class(GTK_WIDGET(d->entry), "invalid-input");
    gtk_range_set_value(GTK_RANGE(d->scale), val);

    char fmt[32];
    if (d->is_exposure) {
      if (fabs(val) < 0.005) snprintf(fmt, sizeof(fmt), "0.00");
      else snprintf(fmt, sizeof(fmt), "%+.2f", val);
    } else {
      int iv = (int)round(val);
      if (iv == 0) snprintf(fmt, sizeof(fmt), "0");
      else snprintf(fmt, sizeof(fmt), "%+d", iv);
    }
    s->is_updating_adj_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(d->entry), fmt);
    s->is_updating_adj_ui = FALSE;
  } else {
    // Validation failure: indicate invalid state with red border and revert to current slider value
    gtk_widget_add_css_class(GTK_WIDGET(d->entry), "invalid-input");
    double cur_val = gtk_range_get_value(GTK_RANGE(d->scale));
    char fmt[32];
    if (d->is_exposure) {
      if (fabs(cur_val) < 0.005) snprintf(fmt, sizeof(fmt), "0.00");
      else snprintf(fmt, sizeof(fmt), "%+.2f", cur_val);
    } else {
      int iv = (int)round(cur_val);
      if (iv == 0) snprintf(fmt, sizeof(fmt), "0");
      else snprintf(fmt, sizeof(fmt), "%+d", iv);
    }
    s->is_updating_adj_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(d->entry), fmt);
    s->is_updating_adj_ui = FALSE;
  }
}

static void on_adj_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  AdjSliderData *d = user_data;
  commit_adj_entry_value(d);
}

static void on_adj_entry_focus_leave(GtkEventControllerFocus *controller, gpointer user_data) {
  (void)controller;
  AdjSliderData *d = user_data;
  commit_adj_entry_value(d);
}

static void on_adj_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)user_data;
  gtk_widget_remove_css_class(GTK_WIDGET(editable), "invalid-input");
}

static gboolean on_adj_entry_key_pressed(GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state, gpointer user_data) {
  (void)controller; (void)keycode;
  AdjSliderData *d = user_data;
  if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
    double cur_val = 0.0;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(d->entry));
    if (!parse_and_validate_adj_value(text, d->min_val, d->max_val, &cur_val)) {
      cur_val = gtk_range_get_value(GTK_RANGE(d->scale));
    }
    double delta = (keyval == GDK_KEY_Up) ? d->step : -d->step;
    if (state & GDK_SHIFT_MASK) {
      delta *= 5.0;
    }
    double new_val = CLAMP(cur_val + delta, d->min_val, d->max_val);
    gtk_range_set_value(GTK_RANGE(d->scale), new_val);

    char fmt[32];
    if (d->is_exposure) {
      if (fabs(new_val) < 0.005) snprintf(fmt, sizeof(fmt), "0.00");
      else snprintf(fmt, sizeof(fmt), "%+.2f", new_val);
    } else {
      int iv = (int)round(new_val);
      if (iv == 0) snprintf(fmt, sizeof(fmt), "0");
      else snprintf(fmt, sizeof(fmt), "%+d", iv);
    }
    d->app->is_updating_adj_ui = TRUE;
    gtk_editable_set_text(GTK_EDITABLE(d->entry), fmt);
    d->app->is_updating_adj_ui = FALSE;
    return TRUE;
  }
  return FALSE;
}

static void on_scale_double_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  (void)x; (void)y;
  if (n_press >= 2) {
    AdjSliderData *d = (AdjSliderData*)user_data;
    if (!d || !d->scale) return;
    GtkScale *scale = d->scale;
    d->last_double_click_time = g_get_monotonic_time();

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // Deny sequence on all other gestures on the scale widget so GtkRange's internal click/drag cancels!
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(scale));
    if (controllers) {
      guint n = g_list_model_get_n_items(controllers);
      for (guint i = 0; i < n; i++) {
        gpointer c = g_list_model_get_item(controllers, i);
        if (GTK_IS_GESTURE(c) && c != gesture) {
          gtk_gesture_set_state(GTK_GESTURE(c), GTK_EVENT_SEQUENCE_DENIED);
        }
        g_object_unref(c);
      }
      g_object_unref(controllers);
    }

    gtk_range_set_value(GTK_RANGE(scale), 0.0);
  }
}

static GtkWidget* create_slider_row(OflApp *s,
                                    const char *label_text,
                                    GtkScale **out_scale,
                                    GtkEntry **out_val_entry,
                                    double min_val, double max_val, double step,
                                    gboolean is_exposure,
                                    GCallback reset_cb) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  AdjSliderData *data = g_new0(AdjSliderData, 1);
  data->app = s;
  data->min_val = min_val;
  data->max_val = max_val;
  data->step = step;
  data->is_exposure = is_exposure;

  GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *lbl = gtk_label_new(label_text);
  gtk_widget_add_css_class(lbl, "adj-control-label");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_box_append(GTK_BOX(top_row), lbl);

  GtkWidget *val_entry = gtk_entry_new();
  gtk_widget_add_css_class(val_entry, "adj-value-entry");
  gtk_widget_set_size_request(val_entry, 56, 22);
  gtk_widget_set_halign(val_entry, GTK_ALIGN_END);
  gtk_entry_set_alignment(GTK_ENTRY(val_entry), 1.0f);
  gtk_editable_set_text(GTK_EDITABLE(val_entry), is_exposure ? "0.00" : "0");
  disable_entry_text_drag(GTK_ENTRY(val_entry));
  data->entry = GTK_ENTRY(val_entry);

  g_signal_connect(val_entry, "activate", G_CALLBACK(on_adj_entry_activate), data);
  g_signal_connect(val_entry, "changed", G_CALLBACK(on_adj_entry_changed), data);

  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "leave", G_CALLBACK(on_adj_entry_focus_leave), data);
  gtk_widget_add_controller(val_entry, focus);

  GtkEventController *key = gtk_event_controller_key_new();
  g_signal_connect(key, "key-pressed", G_CALLBACK(on_adj_entry_key_pressed), data);
  gtk_widget_add_controller(val_entry, key);

  gtk_box_append(GTK_BOX(top_row), val_entry);
  *out_val_entry = GTK_ENTRY(val_entry);

  GtkWidget *btn_rst = gtk_button_new_with_label("↺");
  gtk_widget_add_css_class(btn_rst, "adj-reset-btn");
  gtk_widget_set_tooltip_text(btn_rst, "Reset to 0");
  g_signal_connect(btn_rst, "clicked", reset_cb, s);
  gtk_box_append(GTK_BOX(top_row), btn_rst);

  gtk_box_append(GTK_BOX(row), top_row);

  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min_val, max_val, step);
  gtk_widget_set_hexpand(scale, FALSE);
  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
  gtk_scale_add_mark(GTK_SCALE(scale), 0.0, GTK_POS_BOTTOM, NULL);
  gtk_range_set_value(GTK_RANGE(scale), 0.0);
  gtk_widget_set_tooltip_text(scale, "Double-click to reset to 0");
  data->scale = GTK_SCALE(scale);

  GtkGesture *dbl_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl_click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(dbl_click), GTK_PHASE_CAPTURE);
  g_signal_connect(dbl_click, "pressed", G_CALLBACK(on_scale_double_click), data);
  gtk_widget_add_controller(scale, GTK_EVENT_CONTROLLER(dbl_click));

  g_object_set_data(G_OBJECT(scale), "slider_data", data);

  g_signal_connect(scale, "value-changed", G_CALLBACK(on_adj_slider_changed), s);
  *out_scale = GTK_SCALE(scale);

  gtk_box_append(GTK_BOX(row), scale);

  g_object_set_data_full(G_OBJECT(row), "slider_data", data, g_free);
  return row;
}

static GtkWidget* create_adjustments_panel(OflApp *s) {
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroller, 280, -1);
  gtk_widget_set_hexpand(scroller, FALSE);
  gtk_widget_add_css_class(scroller, "adjustment-sidebar");

  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_hexpand(panel, FALSE);
  gtk_widget_add_css_class(panel, "adjustment-panel");
  gtk_widget_set_margin_start(panel, 14);
  gtk_widget_set_margin_end(panel, 14);
  gtk_widget_set_margin_top(panel, 14);
  gtk_widget_set_margin_bottom(panel, 14);

  // Header row
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl_title = gtk_label_new("Adjustments");
  gtk_widget_add_css_class(lbl_title, "adj-title");
  gtk_widget_set_halign(lbl_title, GTK_ALIGN_START);
  gtk_widget_set_hexpand(lbl_title, TRUE);

  GtkWidget *btn_reset_all = gtk_button_new_with_label("Reset All");
  gtk_widget_add_css_class(btn_reset_all, "adj-reset-all-btn");
  g_signal_connect(btn_reset_all, "clicked", G_CALLBACK(on_reset_all_adjustments_clicked), s);

  gtk_box_append(GTK_BOX(hdr), lbl_title);
  gtk_box_append(GTK_BOX(hdr), btn_reset_all);
  gtk_box_append(GTK_BOX(panel), hdr);

  // Section: WHITE BALANCE
  GtkWidget *sec_wb = gtk_label_new("WHITE BALANCE & TINT");
  gtk_widget_add_css_class(sec_wb, "adj-section-header");
  gtk_widget_set_halign(sec_wb, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(panel), sec_wb);

  // 1- White Balance & Tint
  GtkWidget *row_temp = create_slider_row(s, "Temperature", &s->scale_temp, &s->entry_val_temp,
                                          -100.0, 100.0, 1.0, FALSE, G_CALLBACK(on_reset_temp_clicked));
  gtk_box_append(GTK_BOX(panel), row_temp);

  GtkWidget *row_tint = create_slider_row(s, "Tint", &s->scale_tint, &s->entry_val_tint,
                                          -100.0, 100.0, 1.0, FALSE, G_CALLBACK(on_reset_tint_clicked));
  gtk_box_append(GTK_BOX(panel), row_tint);

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(sep, 4);
  gtk_widget_set_margin_bottom(sep, 4);
  gtk_box_append(GTK_BOX(panel), sep);

  // Section: TONE
  GtkWidget *sec_tone = gtk_label_new("TONE & EXPOSURE");
  gtk_widget_add_css_class(sec_tone, "adj-section-header");
  gtk_widget_set_halign(sec_tone, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(panel), sec_tone);

  // 2- Exposure adjustment (+- 2 stops)
  GtkWidget *row_exp = create_slider_row(s, "Exposure (EV)", &s->scale_exposure, &s->entry_val_exposure,
                                         -2.0, 2.0, 0.05, TRUE, G_CALLBACK(on_reset_exposure_clicked));
  gtk_box_append(GTK_BOX(panel), row_exp);

  // 3- Contrast
  GtkWidget *row_con = create_slider_row(s, "Contrast", &s->scale_contrast, &s->entry_val_contrast,
                                         -100.0, 100.0, 1.0, FALSE, G_CALLBACK(on_reset_contrast_clicked));
  gtk_box_append(GTK_BOX(panel), row_con);

  // 4- Brightness
  GtkWidget *row_bri = create_slider_row(s, "Brightness", &s->scale_brightness, &s->entry_val_brightness,
                                         -100.0, 100.0, 1.0, FALSE, G_CALLBACK(on_reset_brightness_clicked));
  gtk_box_append(GTK_BOX(panel), row_bri);

  // Section: CROP & COMPOSITION
  GtkWidget *sep_crop = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(sep_crop, 4);
  gtk_widget_set_margin_bottom(sep_crop, 4);
  gtk_box_append(GTK_BOX(panel), sep_crop);

  GtkWidget *sec_crop = gtk_label_new("CROP & COMPOSITION");
  gtk_widget_add_css_class(sec_crop, "adj-section-header");
  gtk_widget_set_halign(sec_crop, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(panel), sec_crop);

  GtkWidget *crop_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *btn_reset_crop = gtk_button_new_with_label("Reset Crop");
  gtk_widget_add_css_class(btn_reset_crop, "adj-reset-all-btn");
  g_signal_connect(btn_reset_crop, "clicked", G_CALLBACK(on_reset_crop_clicked), s);
  gtk_box_append(GTK_BOX(crop_hbox), btn_reset_crop);
  gtk_box_append(GTK_BOX(panel), crop_hbox);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), panel);
  s->adj_panel = scroller;
  gtk_widget_set_sensitive(s->adj_panel, FALSE);

  return scroller;
}

static void on_selection_model_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data) {
  (void)position;
  (void)n_items;
  OflApp *s = user_data;
  GtkBitset *bs = gtk_selection_model_get_selection(model);
  if (bs && !gtk_bitset_is_empty(bs)) {
    if (!gtk_bitset_contains(bs, s->active_frame_idx)) {
      s->active_frame_idx = gtk_bitset_get_minimum(bs);
    }
  }
  sync_adjustments_ui_from_selected_frame(s);
  render_selected_preview(s);

  guint total = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (s->btn_export) {
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_export), total > 0);
  }
  if (s->btn_crop) {
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_crop), total > 0);
  }
  if (s->crop_mode && s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
}

static void on_rotate_ccw_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  guint idx = s->active_frame_idx;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!fr) return;

  int rot = ofl_frame_get_rotate_steps(fr);
  ofl_frame_set_rotate_steps(fr, rot - 1);

  const OflCrop *cr = ofl_frame_get_crop(fr);
  if (cr && cr->enabled) {
    OflCrop rc;
    rc.enabled = TRUE;
    rc.x = CLAMP(cr->y, 0.0f, 1.0f);
    rc.y = CLAMP(1.0f - (cr->x + cr->width), 0.0f, 1.0f);
    rc.width = CLAMP(cr->height, 0.01f, 1.0f - rc.x);
    rc.height = CLAMP(cr->width, 0.01f, 1.0f - rc.y);
    ofl_frame_set_crop(fr, &rc);
  }

  g_object_unref(fr);

  if (s->crop_mode && s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
  render_selected_preview(s);
}

static void on_rotate_cw_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  guint idx = s->active_frame_idx;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!fr) return;

  int rot = ofl_frame_get_rotate_steps(fr);
  ofl_frame_set_rotate_steps(fr, rot + 1);

  const OflCrop *cr = ofl_frame_get_crop(fr);
  if (cr && cr->enabled) {
    OflCrop rc;
    rc.enabled = TRUE;
    rc.x = CLAMP(1.0f - (cr->y + cr->height), 0.0f, 1.0f);
    rc.y = CLAMP(cr->x, 0.0f, 1.0f);
    rc.width = CLAMP(cr->height, 0.01f, 1.0f - rc.x);
    rc.height = CLAMP(cr->width, 0.01f, 1.0f - rc.y);
    ofl_frame_set_crop(fr, &rc);
  }

  g_object_unref(fr);

  if (s->crop_mode && s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
  render_selected_preview(s);
}

static void on_pick_base_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;
  if (s->crop_mode) {
    s->crop_mode = FALSE;
    if (s->btn_crop) gtk_widget_remove_css_class(GTK_WIDGET(s->btn_crop), "suggested-action");
    if (s->crop_overlay) gtk_widget_set_visible(s->crop_overlay, FALSE);
    render_selected_preview(s);
  }
  if (s->pick_base_mode) {
    // Cancel pick mode
    s->pick_base_mode = FALSE;
    gtk_widget_set_cursor(GTK_WIDGET(s->preview), NULL);
    gtk_button_set_label(s->btn_pick, "Pick Film Base");
    gtk_widget_remove_css_class(GTK_WIDGET(s->btn_pick), "suggested-action");
    gtk_widget_set_visible(s->hud_box, FALSE);
    if (s->has_roll_base) {
      render_selected_preview(s);
    }
    return;
  }

  s->pick_base_mode = TRUE;
  gtk_button_set_label(s->btn_pick, "Cancel Pick");
  gtk_widget_add_css_class(GTK_WIDGET(s->btn_pick), "suggested-action");

  GdkCursor *fallback = gdk_cursor_new_from_name("crosshair", NULL);
  GdkCursor *cur = gdk_cursor_new_from_name("eyedropper", fallback);
  if (!cur) cur = gdk_cursor_new_from_name("color-picker", fallback);
  gtk_widget_set_cursor(GTK_WIDGET(s->preview), cur ? cur : fallback);
  if (cur) g_object_unref(cur);
  if (fallback) g_object_unref(fallback);

  gtk_widget_set_visible(s->hud_box, TRUE);
  char msg[128];
  if (s->has_picked_base) {
    int r = (int)(CLAMP(s->display_rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    int g = (int)(CLAMP(s->display_rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    int b = (int)(CLAMP(s->display_rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    snprintf(msg, sizeof(msg), "Custom Base: R: %d  G: %d  B: %d  (Click image to re-sample)", r, g, b);
  } else {
    snprintf(msg, sizeof(msg), "Default Base: R: %d  G: %d  B: %d  (Click image to sample custom)",
             OFL_DEFAULT_BASE_8BIT_R, OFL_DEFAULT_BASE_8BIT_G, OFL_DEFAULT_BASE_8BIT_B);
  }
  gtk_label_set_text(GTK_LABEL(s->hud_label), msg);
  gtk_widget_queue_draw(s->hud_swatch);

  // If already converted to positive, show negative so user can inspect and sample the orange mask
  if (s->has_roll_base) {
    render_selected_preview(s);
  }
}

static void on_apply_roll_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;

  // We have a base available (either default R:247 G:203 B:172 or user-picked override)
  s->has_roll_base = TRUE;
  gtk_widget_remove_css_class(GTK_WIDGET(s->btn_apply), "suggested-action");

  // Hide the RGB HUD pill now that roll conversion is applied
  gtk_widget_set_visible(s->hud_box, FALSE);

  int r = (int)(CLAMP(s->display_rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
  int g = (int)(CLAMP(s->display_rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
  int b = (int)(CLAMP(s->display_rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
  g_message("Applying roll conversion with %s film base: 8-bit=(%d, %d, %d) raw=(%.4f, %.4f, %.4f)",
            s->has_picked_base ? "user-picked" : "default",
            r, g, b,
            s->roll_base_rgb[0], s->roll_base_rgb[1], s->roll_base_rgb[2]);

  // 1. Invert and render the active preview immediately
  render_selected_preview(s);

  // 2. Start background thumbnail conversion for all other frames in the roll
  start_roll_thumbnails_conversion(s);
}

// Async callback for open folder dialog
static void on_open_folder_finish(GObject *source_object, GAsyncResult *res, gpointer data) {
  OflApp *s = data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);

  GError *err = NULL;
  GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &err);
  if (!folder) {
    if (err) g_clear_error(&err);
    return;
  }

  scan_folder(s, folder);
  g_object_unref(folder);

  // select first frame if any
  guint n = g_list_model_get_n_items(G_LIST_MODEL(s->frames));
  if (n > 0) {
    s->active_frame_idx = 0;
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(s->sel), 0, TRUE);
  }
  if (s->btn_export) {
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_export), n > 0);
  }
  if (s->btn_crop) {
    gtk_widget_set_sensitive(GTK_WIDGET(s->btn_crop), n > 0);
  }
}

static void on_open_folder_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open Roll Folder");

  gtk_file_dialog_select_folder(
    dlg,
    GTK_WINDOW(s->win),
    NULL,
    on_open_folder_finish,
    s
  );
  g_object_unref(dlg);
}

// ==================== Export Dialog & Worker ====================

typedef struct {
  OflApp *app;
  GtkWidget *dialog_win;

  GArray *selected_indices;
  guint total_frames;

  GtkCheckButton *chk_scope_selected;
  GtkCheckButton *chk_scope_all;

  GtkEntry *entry_dest;
  GtkWidget *btn_choose_dest;

  GtkEntry *entry_pattern;
  GtkLabel *lbl_preview_filename;

  GtkCheckButton *rb_jpeg;
  GtkCheckButton *rb_tiff;
  GtkWidget *box_jpeg_options;
  GtkWidget *box_tiff_options;

  GtkScale *scale_jpeg_quality;
  GtkLabel *lbl_jpeg_quality_val;
  GtkDropDown *dd_jpeg_profile;
  GtkSwitch *sw_jpeg_embed;

  GtkDropDown *dd_tiff_depth;
  GtkDropDown *dd_tiff_profile;
  GtkSwitch *sw_tiff_embed;

  GtkWidget *btn_cancel;
  GtkWidget *btn_start_export;
  GtkWidget *form_box;
  GtkWidget *progress_box;
  GtkProgressBar *progress_bar;
  GtkLabel *lbl_progress_status;
  GtkWidget *done_box;
  GtkLabel *lbl_done_msg;
  GtkWidget *btn_reveal_finder;
  GtkWidget *btn_done_close;

  gboolean is_exporting;
  char *output_directory;
} ExportDialogState;

typedef struct {
  ExportDialogState *dlg;
  guint current;
  guint total;
  char *filename;
} ExportProgressMsg;

typedef struct {
  ExportDialogState *dlg;
  guint exported_count;
  char *output_dir;
  gboolean success;
  char *error_msg;
} ExportFinishedMsg;

typedef struct {
  ExportDialogState *dlg;
  GArray *indices; // array of guint
  OflExportSettings settings;
  float roll_base[3];
  OflNegParams neg;
  GListStore *frames;
} ExportTaskData;

static GArray* get_selected_frame_indices(OflApp *s) {
  GArray *arr = g_array_new(FALSE, FALSE, sizeof(guint));
  if (!s || !s->sel) return arr;

  GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(s->sel));
  if (bs && !gtk_bitset_is_empty(bs)) {
    GtkBitsetIter iter;
    guint val;
    if (gtk_bitset_iter_init_first(&iter, bs, &val)) {
      do {
        g_array_append_val(arr, val);
      } while (gtk_bitset_iter_next(&iter, &val));
    }
  } else {
    guint total = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
    if (total > 0 && s->active_frame_idx < total) {
      g_array_append_val(arr, s->active_frame_idx);
    }
  }
  return arr;
}

static void update_export_preview_filename(ExportDialogState *st) {
  if (!st || !st->entry_pattern || !st->lbl_preview_filename) return;

  const char *pat = gtk_editable_get_text(GTK_EDITABLE(st->entry_pattern));
  OflExportFormat fmt = gtk_check_button_get_active(st->rb_jpeg) ? OFL_EXPORT_FORMAT_JPEG : OFL_EXPORT_FORMAT_TIFF;

  const char *sample_path = NULL;
  guint idx = 0;
  if (st->selected_indices && st->selected_indices->len > 0) {
    idx = g_array_index(st->selected_indices, guint, 0);
  } else {
    idx = st->app->active_frame_idx;
  }
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(st->app->frames), idx);
  if (fr) {
    GFile *f = ofl_frame_get_file(fr);
    if (f) sample_path = g_file_get_path(f);
    g_object_unref(fr);
  }

  char *result = ofl_export_format_filename(pat, sample_path, 1, fmt);
  char *markup = g_strdup_printf("Preview: <span font_family=\"monospace\" color=\"#60a5fa\">%s</span>", result ? result : "");
  gtk_label_set_markup(st->lbl_preview_filename, markup);
  g_free(markup);
  g_free(result);
  if (sample_path) g_free((char*)sample_path);
}

static void on_token_chip_clicked(GtkButton *btn, gpointer user_data) {
  ExportDialogState *st = user_data;
  const char *token = gtk_button_get_label(btn);
  const char *curr = gtk_editable_get_text(GTK_EDITABLE(st->entry_pattern));
  char *new_text;
  if (!curr || *curr == '\0') {
    new_text = g_strdup(token);
  } else {
    new_text = g_strdup_printf("%s_%s", curr, token);
  }
  gtk_editable_set_text(GTK_EDITABLE(st->entry_pattern), new_text);
  g_free(new_text);
}

static void on_export_format_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  ExportDialogState *st = user_data;
  gboolean is_jpg = gtk_check_button_get_active(st->rb_jpeg);
  gtk_widget_set_visible(st->box_jpeg_options, is_jpg);
  gtk_widget_set_visible(st->box_tiff_options, !is_jpg);
  update_export_preview_filename(st);
}

static void on_export_quality_changed(GtkRange *range, gpointer user_data) {
  ExportDialogState *st = user_data;
  int val = (int)gtk_range_get_value(range);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", val);
  gtk_label_set_text(st->lbl_jpeg_quality_val, buf);
}

static void on_export_choose_dest_finish(GObject *source_object, GAsyncResult *res, gpointer user_data) {
  ExportDialogState *st = user_data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
  GError *err = NULL;
  GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &err);
  if (folder) {
    char *path = g_file_get_path(folder);
    if (path) {
      gtk_editable_set_text(GTK_EDITABLE(st->entry_dest), path);
      g_free(path);
    }
    g_object_unref(folder);
  }
  if (err) g_clear_error(&err);
}

static void on_export_choose_dest_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ExportDialogState *st = user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Select Export Destination");
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(st->dialog_win), NULL, on_export_choose_dest_finish, st);
  g_object_unref(dlg);
}

static void on_reveal_in_finder_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ExportDialogState *st = user_data;
  if (st->output_directory) {
    GFile *f = g_file_new_for_path(st->output_directory);
    char *uri = g_file_get_uri(f);
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
    g_object_unref(f);
  }
}

static gboolean on_export_progress_idle(gpointer user_data) {
  ExportProgressMsg *msg = user_data;
  if (!msg) return G_SOURCE_REMOVE;
  ExportDialogState *dlg = msg->dlg;

  if (GTK_IS_WIDGET(dlg->dialog_win)) {
    double frac = (msg->total > 0) ? (double)msg->current / (double)msg->total : 0.0;
    gtk_progress_bar_set_fraction(dlg->progress_bar, frac);

    char buf[256];
    snprintf(buf, sizeof(buf), "Exported %u of %u: %s", msg->current, msg->total, msg->filename ? msg->filename : "");
    gtk_label_set_text(dlg->lbl_progress_status, buf);
  }

  g_free(msg->filename);
  g_free(msg);
  return G_SOURCE_REMOVE;
}

static gboolean on_export_finished_idle(gpointer user_data) {
  ExportFinishedMsg *msg = user_data;
  if (!msg) return G_SOURCE_REMOVE;
  ExportDialogState *dlg = msg->dlg;

  if (GTK_IS_WIDGET(dlg->dialog_win)) {
    gtk_widget_set_visible(dlg->progress_box, FALSE);
    gtk_widget_set_visible(dlg->done_box, TRUE);

    if (msg->success) {
      char buf[256];
      snprintf(buf, sizeof(buf), "✓ Export completed! %u images saved.", msg->exported_count);
      gtk_label_set_text(dlg->lbl_done_msg, buf);
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "Export completed with issues: %s", msg->error_msg ? msg->error_msg : "Check folder");
      gtk_label_set_text(dlg->lbl_done_msg, buf);
    }
  }

  dlg->is_exporting = FALSE;
  if (GTK_IS_WIDGET(dlg->btn_cancel)) {
    gtk_widget_set_sensitive(dlg->btn_cancel, TRUE);
  }
  g_free(msg->output_dir);
  g_free(msg->error_msg);
  g_free(msg);
  return G_SOURCE_REMOVE;
}

static gpointer export_thread_func(gpointer user_data) {
  ExportTaskData *task = user_data;
  guint n_items = task->indices->len;
  guint success_count = 0;
  gboolean all_ok = TRUE;
  char *err_msg = NULL;

  for (guint i = 0; i < n_items; i++) {
    guint idx = g_array_index(task->indices, guint, i);
    OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(task->frames), idx);
    if (!fr) continue;

    GError *err = NULL;
    char *saved_path = NULL;
    gboolean ok = ofl_export_frame(fr, task->roll_base, &task->neg, &task->settings, i + 1, &saved_path, &err);

    char *fname = saved_path ? g_path_get_basename(saved_path) : NULL;
    g_free(saved_path);

    if (ok) {
      success_count++;
    } else {
      all_ok = FALSE;
      if (!err_msg && err) {
        err_msg = g_strdup(err->message);
      }
      if (err) g_clear_error(&err);
    }

    g_object_unref(fr);

    ExportProgressMsg *pmsg = g_new0(ExportProgressMsg, 1);
    pmsg->dlg = task->dlg;
    pmsg->current = i + 1;
    pmsg->total = n_items;
    pmsg->filename = fname;
    g_idle_add(on_export_progress_idle, pmsg);
  }

  ExportFinishedMsg *fmsg = g_new0(ExportFinishedMsg, 1);
  fmsg->dlg = task->dlg;
  fmsg->exported_count = success_count;
  fmsg->output_dir = g_strdup(task->settings.output_dir);
  fmsg->success = all_ok;
  fmsg->error_msg = err_msg;
  g_idle_add(on_export_finished_idle, fmsg);

  g_array_unref(task->indices);
  ofl_export_settings_clear(&task->settings);
  g_object_unref(task->frames);
  g_free(task);

  return NULL;
}

static void on_export_dialog_start(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ExportDialogState *st = user_data;
  if (st->is_exporting) return;

  const char *dest = gtk_editable_get_text(GTK_EDITABLE(st->entry_dest));
  if (!dest || *dest == '\0') {
    return;
  }

  GArray *target_indices = g_array_new(FALSE, FALSE, sizeof(guint));
  if (gtk_check_button_get_active(st->chk_scope_selected) && st->selected_indices->len > 0) {
    for (guint i = 0; i < st->selected_indices->len; i++) {
      guint val = g_array_index(st->selected_indices, guint, i);
      g_array_append_val(target_indices, val);
    }
  } else {
    for (guint i = 0; i < st->total_frames; i++) {
      g_array_append_val(target_indices, i);
    }
  }

  if (target_indices->len == 0) {
    g_array_unref(target_indices);
    return;
  }

  st->is_exporting = TRUE;
  g_free(st->output_directory);
  st->output_directory = g_strdup(dest);

  ExportTaskData *task = g_new0(ExportTaskData, 1);
  task->dlg = st;
  task->indices = target_indices;
  ofl_export_settings_init_defaults(&task->settings);
  task->settings.output_dir = g_strdup(dest);
  task->settings.naming_pattern = g_strdup(gtk_editable_get_text(GTK_EDITABLE(st->entry_pattern)));
  task->settings.format = gtk_check_button_get_active(st->rb_jpeg) ? OFL_EXPORT_FORMAT_JPEG : OFL_EXPORT_FORMAT_TIFF;

  if (task->settings.format == OFL_EXPORT_FORMAT_JPEG) {
    task->settings.jpeg_quality = (int)gtk_range_get_value(GTK_RANGE(st->scale_jpeg_quality));
    task->settings.profile = (OflColorProfile)gtk_drop_down_get_selected(st->dd_jpeg_profile);
    task->settings.embed_profile = gtk_switch_get_active(st->sw_jpeg_embed);
  } else {
    task->settings.tiff_depth = (OflTiffDepth)gtk_drop_down_get_selected(st->dd_tiff_depth);
    task->settings.profile = (OflColorProfile)gtk_drop_down_get_selected(st->dd_tiff_profile);
    task->settings.embed_profile = gtk_switch_get_active(st->sw_tiff_embed);
  }

  task->roll_base[0] = st->app->roll_base_rgb[0];
  task->roll_base[1] = st->app->roll_base_rgb[1];
  task->roll_base[2] = st->app->roll_base_rgb[2];
  task->neg = st->app->neg;
  task->frames = g_object_ref(st->app->frames);

  // Switch UI to exporting state
  gtk_widget_set_sensitive(st->btn_start_export, FALSE);
  gtk_widget_set_sensitive(st->btn_cancel, FALSE);
  gtk_widget_set_sensitive(st->form_box, FALSE);

  gtk_widget_set_visible(st->progress_box, TRUE);
  gtk_progress_bar_set_fraction(st->progress_bar, 0.0);
  gtk_label_set_text(st->lbl_progress_status, "Starting export...");

  GThread *th = g_thread_new("ofl_export_worker", export_thread_func, task);
  g_thread_unref(th);
}

static gboolean on_export_dialog_close_request(GtkWindow *win, gpointer user_data) {
  (void)win;
  ExportDialogState *st = user_data;
  if (st && st->is_exporting) {
    return TRUE; // Do not close while export is actively in progress
  }
  return FALSE;
}

static void on_export_dialog_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ExportDialogState *st = user_data;
  if (st->is_exporting) return;
  gtk_window_destroy(GTK_WINDOW(st->dialog_win));
}

static void on_export_dialog_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  ExportDialogState *st = user_data;
  if (!st) return;
  if (st->selected_indices) g_array_unref(st->selected_indices);
  g_free(st->output_directory);
  g_free(st);
}

static void show_export_dialog(OflApp *s) {
  guint total = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (total == 0) return;

  ExportDialogState *st = g_new0(ExportDialogState, 1);
  st->app = s;
  st->total_frames = total;
  st->selected_indices = get_selected_frame_indices(s);
  st->is_exporting = FALSE;

  GtkWidget *win = gtk_window_new();
  st->dialog_win = win;
  gtk_window_set_title(GTK_WINDOW(win), "Export Images");
  gtk_window_set_transient_for(GTK_WINDOW(win), s->win);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(win), 560, 680);
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  gtk_widget_add_css_class(win, "dark-window");

  g_signal_connect(win, "close-request", G_CALLBACK(on_export_dialog_close_request), st);
  g_signal_connect(win, "destroy", G_CALLBACK(on_export_dialog_destroy), st);

  // Headerbar
  GtkWidget *hb = gtk_header_bar_new();
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);

  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  st->btn_cancel = btn_cancel;
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_cancel);
  g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_export_dialog_cancel), st);

  GtkWidget *btn_export = gtk_button_new_with_label("Export");
  st->btn_start_export = btn_export;
  gtk_widget_add_css_class(btn_export, "suggested-action");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_export);
  g_signal_connect(btn_export, "clicked", G_CALLBACK(on_export_dialog_start), st);

  // Main scroll & container
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_window_set_child(GTK_WINDOW(win), scroller);

  GtkWidget *root_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start(root_vbox, 20);
  gtk_widget_set_margin_end(root_vbox, 20);
  gtk_widget_set_margin_top(root_vbox, 16);
  gtk_widget_set_margin_bottom(root_vbox, 20);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), root_vbox);

  GtkWidget *form_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  st->form_box = form_box;
  gtk_box_append(GTK_BOX(root_vbox), form_box);

  // ---------- 1. Scope Section ----------
  GtkWidget *card_scope = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(card_scope, "export-card");

  GtkWidget *lbl_scope_title = gtk_label_new("FRAMES TO EXPORT");
  gtk_widget_add_css_class(lbl_scope_title, "export-section-title");
  gtk_label_set_xalign(GTK_LABEL(lbl_scope_title), 0.0f);
  gtk_box_append(GTK_BOX(card_scope), lbl_scope_title);

  guint n_sel = st->selected_indices ? st->selected_indices->len : 0;
  char sel_label[64];
  if (n_sel <= 1) {
    snprintf(sel_label, sizeof(sel_label), "Selected frame (1 frame)");
  } else {
    snprintf(sel_label, sizeof(sel_label), "Selected frames (%u frames)", n_sel);
  }

  char all_label[64];
  snprintf(all_label, sizeof(all_label), "All frames in roll (%u frames)", total);

  GtkWidget *chk_sel = gtk_check_button_new_with_label(sel_label);
  GtkWidget *chk_all = gtk_check_button_new_with_label(all_label);
  st->chk_scope_selected = GTK_CHECK_BUTTON(chk_sel);
  st->chk_scope_all = GTK_CHECK_BUTTON(chk_all);
  gtk_check_button_set_group(GTK_CHECK_BUTTON(chk_all), GTK_CHECK_BUTTON(chk_sel));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_sel), TRUE);

  gtk_box_append(GTK_BOX(card_scope), chk_sel);
  gtk_box_append(GTK_BOX(card_scope), chk_all);
  gtk_box_append(GTK_BOX(form_box), card_scope);

  // ---------- 2. Destination Section ----------
  GtkWidget *card_dest = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(card_dest, "export-card");

  GtkWidget *lbl_dest_title = gtk_label_new("DESTINATION FOLDER");
  gtk_widget_add_css_class(lbl_dest_title, "export-section-title");
  gtk_label_set_xalign(GTK_LABEL(lbl_dest_title), 0.0f);
  gtk_box_append(GTK_BOX(card_dest), lbl_dest_title);

  GtkWidget *dest_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *entry_dest = gtk_entry_new();
  st->entry_dest = GTK_ENTRY(entry_dest);
  gtk_widget_set_hexpand(entry_dest, TRUE);

  char *def_dest = NULL;
  if (s->current_roll_folder) {
    char *roll_path = g_file_get_path(s->current_roll_folder);
    if (roll_path) {
      def_dest = g_build_filename(roll_path, "Export", NULL);
      g_free(roll_path);
    }
  }
  if (!def_dest) {
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    def_dest = g_build_filename(pics ? pics : g_get_home_dir(), "OpenFilmLab_Export", NULL);
  }
  gtk_editable_set_text(GTK_EDITABLE(entry_dest), def_dest);
  g_free(def_dest);

  GtkWidget *btn_browse = gtk_button_new_with_label("Choose…");
  st->btn_choose_dest = btn_browse;
  g_signal_connect(btn_browse, "clicked", G_CALLBACK(on_export_choose_dest_clicked), st);

  gtk_box_append(GTK_BOX(dest_hbox), entry_dest);
  gtk_box_append(GTK_BOX(dest_hbox), btn_browse);
  gtk_box_append(GTK_BOX(card_dest), dest_hbox);
  gtk_box_append(GTK_BOX(form_box), card_dest);

  // ---------- 3. File Naming Section ----------
  GtkWidget *card_naming = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(card_naming, "export-card");

  GtkWidget *lbl_naming_title = gtk_label_new("FILE NAMING PATTERN");
  gtk_widget_add_css_class(lbl_naming_title, "export-section-title");
  gtk_label_set_xalign(GTK_LABEL(lbl_naming_title), 0.0f);
  gtk_box_append(GTK_BOX(card_naming), lbl_naming_title);

  GtkWidget *entry_pattern = gtk_entry_new();
  st->entry_pattern = GTK_ENTRY(entry_pattern);
  gtk_editable_set_text(GTK_EDITABLE(entry_pattern), "{name}");
  gtk_box_append(GTK_BOX(card_naming), entry_pattern);

  GtkWidget *chip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const char *tokens[] = { "{name}", "{date}", "{index}", "{time}" };
  for (int i = 0; i < 4; i++) {
    GtkWidget *chip = gtk_button_new_with_label(tokens[i]);
    gtk_widget_add_css_class(chip, "token-chip");
    g_signal_connect(chip, "clicked", G_CALLBACK(on_token_chip_clicked), st);
    gtk_box_append(GTK_BOX(chip_box), chip);
  }
  gtk_box_append(GTK_BOX(card_naming), chip_box);

  GtkWidget *lbl_prev = gtk_label_new(NULL);
  st->lbl_preview_filename = GTK_LABEL(lbl_prev);
  gtk_label_set_xalign(GTK_LABEL(lbl_prev), 0.0f);
  gtk_box_append(GTK_BOX(card_naming), lbl_prev);

  g_signal_connect_swapped(entry_pattern, "notify::text", G_CALLBACK(update_export_preview_filename), st);

  gtk_box_append(GTK_BOX(form_box), card_naming);

  // ---------- 4. Format & Options Section ----------
  GtkWidget *card_format = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class(card_format, "export-card");

  GtkWidget *lbl_fmt_title = gtk_label_new("FILE FORMAT & SETTINGS");
  gtk_widget_add_css_class(lbl_fmt_title, "export-section-title");
  gtk_label_set_xalign(GTK_LABEL(lbl_fmt_title), 0.0f);
  gtk_box_append(GTK_BOX(card_format), lbl_fmt_title);

  GtkWidget *fmt_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *rb_jpg = gtk_check_button_new_with_label("JPEG Image");
  GtkWidget *rb_tif = gtk_check_button_new_with_label("TIFF Image");
  st->rb_jpeg = GTK_CHECK_BUTTON(rb_jpg);
  st->rb_tiff = GTK_CHECK_BUTTON(rb_tif);
  gtk_check_button_set_group(GTK_CHECK_BUTTON(rb_tif), GTK_CHECK_BUTTON(rb_jpg));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(rb_jpg), TRUE);
  g_signal_connect(rb_jpg, "toggled", G_CALLBACK(on_export_format_toggled), st);
  gtk_box_append(GTK_BOX(fmt_hbox), rb_jpg);
  gtk_box_append(GTK_BOX(fmt_hbox), rb_tif);
  gtk_box_append(GTK_BOX(card_format), fmt_hbox);

  // --- JPEG Options Box ---
  GtkWidget *box_jpg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  st->box_jpeg_options = box_jpg;

  GtkWidget *q_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_q_title = gtk_label_new("Compression Quality:");
  gtk_widget_set_hexpand(lbl_q_title, FALSE);
  GtkWidget *lbl_q_val = gtk_label_new("92%");
  st->lbl_jpeg_quality_val = GTK_LABEL(lbl_q_val);

  GtkWidget *scale_q = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10.0, 100.0, 1.0);
  st->scale_jpeg_quality = GTK_SCALE(scale_q);
  gtk_range_set_value(GTK_RANGE(scale_q), 92.0);
  gtk_widget_set_hexpand(scale_q, TRUE);
  g_signal_connect(scale_q, "value-changed", G_CALLBACK(on_export_quality_changed), st);

  gtk_box_append(GTK_BOX(q_hbox), lbl_q_title);
  gtk_box_append(GTK_BOX(q_hbox), scale_q);
  gtk_box_append(GTK_BOX(q_hbox), lbl_q_val);
  gtk_box_append(GTK_BOX(box_jpg), q_hbox);

  GtkWidget *prof_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_prof = gtk_label_new("Color Profile:");
  const char *const profiles[] = {
    "sRGB (Standard Web & Display)",
    "Adobe RGB (1998) (Photography & Print)",
    "Display P3 (Wide Gamut / Apple)",
    NULL
  };
  GtkWidget *dd_jpg_prof = gtk_drop_down_new_from_strings(profiles);
  st->dd_jpeg_profile = GTK_DROP_DOWN(dd_jpg_prof);
  gtk_widget_set_hexpand(dd_jpg_prof, TRUE);
  gtk_box_append(GTK_BOX(prof_hbox), lbl_prof);
  gtk_box_append(GTK_BOX(prof_hbox), dd_jpg_prof);
  gtk_box_append(GTK_BOX(box_jpg), prof_hbox);

  GtkWidget *emb_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_emb = gtk_label_new("Embed Color Profile:");
  gtk_widget_set_hexpand(lbl_emb, TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl_emb), 0.0f);
  GtkWidget *sw_jpg_emb = gtk_switch_new();
  st->sw_jpeg_embed = GTK_SWITCH(sw_jpg_emb);
  gtk_switch_set_active(GTK_SWITCH(sw_jpg_emb), TRUE);
  gtk_box_append(GTK_BOX(emb_hbox), lbl_emb);
  gtk_box_append(GTK_BOX(emb_hbox), sw_jpg_emb);
  gtk_box_append(GTK_BOX(box_jpg), emb_hbox);

  gtk_box_append(GTK_BOX(card_format), box_jpg);

  // --- TIFF Options Box ---
  GtkWidget *box_tif = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  st->box_tiff_options = box_tif;
  gtk_widget_set_visible(box_tif, FALSE);

  GtkWidget *depth_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_depth = gtk_label_new("Bit Depth:");
  const char *const tiff_depths[] = {
    "8-bit per channel",
    "16-bit per channel (Archival / High Dynamic Range)",
    NULL
  };
  GtkWidget *dd_depth = gtk_drop_down_new_from_strings(tiff_depths);
  st->dd_tiff_depth = GTK_DROP_DOWN(dd_depth);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd_depth), 1);
  gtk_widget_set_hexpand(dd_depth, TRUE);
  gtk_box_append(GTK_BOX(depth_hbox), lbl_depth);
  gtk_box_append(GTK_BOX(depth_hbox), dd_depth);
  gtk_box_append(GTK_BOX(box_tif), depth_hbox);

  GtkWidget *tif_prof_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_tif_prof = gtk_label_new("Color Profile:");
  GtkWidget *dd_tif_prof = gtk_drop_down_new_from_strings(profiles);
  st->dd_tiff_profile = GTK_DROP_DOWN(dd_tif_prof);
  gtk_widget_set_hexpand(dd_tif_prof, TRUE);
  gtk_box_append(GTK_BOX(tif_prof_hbox), lbl_tif_prof);
  gtk_box_append(GTK_BOX(tif_prof_hbox), dd_tif_prof);
  gtk_box_append(GTK_BOX(box_tif), tif_prof_hbox);

  GtkWidget *tif_emb_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *lbl_tif_emb = gtk_label_new("Embed Color Profile:");
  gtk_widget_set_hexpand(lbl_tif_emb, TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl_tif_emb), 0.0f);
  GtkWidget *sw_tif_emb = gtk_switch_new();
  st->sw_tiff_embed = GTK_SWITCH(sw_tif_emb);
  gtk_switch_set_active(GTK_SWITCH(sw_tif_emb), TRUE);
  gtk_box_append(GTK_BOX(tif_emb_hbox), lbl_tif_emb);
  gtk_box_append(GTK_BOX(tif_emb_hbox), sw_tif_emb);
  gtk_box_append(GTK_BOX(box_tif), tif_emb_hbox);

  gtk_box_append(GTK_BOX(card_format), box_tif);

  gtk_box_append(GTK_BOX(form_box), card_format);

  // ---------- 5. Progress & Results Section ----------
  GtkWidget *progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  st->progress_box = progress_box;
  gtk_widget_set_visible(progress_box, FALSE);
  gtk_widget_add_css_class(progress_box, "export-card");

  GtkWidget *pbar = gtk_progress_bar_new();
  st->progress_bar = GTK_PROGRESS_BAR(pbar);
  gtk_box_append(GTK_BOX(progress_box), pbar);

  GtkWidget *lbl_pstatus = gtk_label_new("Exporting...");
  st->lbl_progress_status = GTK_LABEL(lbl_pstatus);
  gtk_label_set_xalign(GTK_LABEL(lbl_pstatus), 0.0f);
  gtk_box_append(GTK_BOX(progress_box), lbl_pstatus);

  gtk_box_append(GTK_BOX(root_vbox), progress_box);

  // Done Box
  GtkWidget *done_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  st->done_box = done_box;
  gtk_widget_set_visible(done_box, FALSE);
  gtk_widget_add_css_class(done_box, "export-card");

  GtkWidget *lbl_done = gtk_label_new("✓ Export complete!");
  st->lbl_done_msg = GTK_LABEL(lbl_done);
  gtk_label_set_xalign(GTK_LABEL(lbl_done), 0.0f);
  gtk_box_append(GTK_BOX(done_box), lbl_done);

  GtkWidget *done_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *btn_reveal = gtk_button_new_with_label("Reveal in Finder");
  st->btn_reveal_finder = btn_reveal;
  g_signal_connect(btn_reveal, "clicked", G_CALLBACK(on_reveal_in_finder_clicked), st);

  GtkWidget *btn_done = gtk_button_new_with_label("Done");
  st->btn_done_close = btn_done;
  gtk_widget_add_css_class(btn_done, "suggested-action");
  g_signal_connect(btn_done, "clicked", G_CALLBACK(on_export_dialog_cancel), st);

  gtk_box_append(GTK_BOX(done_actions), btn_reveal);
  gtk_box_append(GTK_BOX(done_actions), btn_done);
  gtk_box_append(GTK_BOX(done_box), done_actions);

  gtk_box_append(GTK_BOX(root_vbox), done_box);

  update_export_preview_filename(st);

  gtk_window_present(GTK_WINDOW(win));
}

static void on_export_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;
  show_export_dialog(s);
}

static void on_preview_pressed(GtkGestureClick *gesture,
                               int n_press, double x, double y,
                               gpointer user_data) {
  (void)gesture;
  (void)n_press;
  OflApp *s = user_data;
  if (!s->pick_base_mode) return;
  if (!s->last_img) {
    g_message("No image decoded yet. Select a frame first.");
    return;
  }

  int ix, iy;
  if (!get_current_image_pixel(s, x, y, &ix, &iy)) {
    g_message("Click inside the image area.");
    return;
  }

  sample_base_at(s->last_img, ix, iy, s->roll_base_rgb);
  s->has_picked_base = TRUE;
  s->pick_base_mode = FALSE;

  gtk_widget_set_cursor(GTK_WIDGET(s->preview), NULL);
  gtk_button_set_label(s->btn_pick, "Pick Film Base");
  gtk_widget_remove_css_class(GTK_WIDGET(s->btn_pick), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(s->btn_apply), "suggested-action");

  float w_pt = (s->preview_white_pt > 1000) ? (float)s->preview_white_pt : 65535.0f;
  const float inv_gamma = 1.0f / 2.2f;
  s->display_rgb[0] = powf(CLAMP(s->roll_base_rgb[0] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  s->display_rgb[1] = powf(CLAMP(s->roll_base_rgb[1] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  s->display_rgb[2] = powf(CLAMP(s->roll_base_rgb[2] * 65535.0f / w_pt, 0.0f, 1.0f), inv_gamma);
  gtk_widget_queue_draw(s->hud_swatch);

  int r = (int)(CLAMP(s->display_rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
  int g = (int)(CLAMP(s->display_rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
  int b = (int)(CLAMP(s->display_rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);

  char buf[128];
  snprintf(buf, sizeof(buf), "Custom Base: R: %d  G: %d  B: %d  → Click 'Apply to Roll'", r, g, b);
  gtk_label_set_text(GTK_LABEL(s->hud_label), buf);
  gtk_widget_set_visible(s->hud_box, TRUE);

  g_message("Overrode film base with user pick: 8-bit=(%d, %d, %d) raw=(%.4f, %.4f, %.4f)",
            r, g, b,
            s->roll_base_rgb[0], s->roll_base_rgb[1], s->roll_base_rgb[2]);
  // Note: We do NOT convert immediately on pick; user clicks 'Apply to Roll' to convert.
}

static void on_list_item_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
  (void)gesture; (void)n_press; (void)x; (void)y;
  GtkListItem *item = GTK_LIST_ITEM(user_data);
  OflApp *s = g_object_get_data(G_OBJECT(item), "app_ptr");
  guint pos = gtk_list_item_get_position(item);
  if (s && pos != GTK_INVALID_LIST_POSITION && s->frames &&
      pos < g_list_model_get_n_items(G_LIST_MODEL(s->frames))) {
    s->active_frame_idx = pos;
    sync_adjustments_ui_from_selected_frame(s);
    render_selected_preview(s);
  }
}

// ---------- List item factory ----------
static void setup_list_item(GtkListItemFactory *factory, GtkListItem *item, gpointer user_data) {
  (void)factory;
  OflApp *s = user_data;
  g_object_set_data(G_OBJECT(item), "app_ptr", s);

  GtkWidget *pic = gtk_picture_new();
  gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);

  gtk_widget_set_size_request(pic, 110, 110);

  gtk_widget_set_margin_start(pic, 8);
  gtk_widget_set_margin_end(pic, 8);
  gtk_widget_set_margin_top(pic, 8);
  gtk_widget_set_margin_bottom(pic, 8);

  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect(click, "pressed", G_CALLBACK(on_list_item_pressed), item);
  gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click));

  gtk_list_item_set_child(item, pic);
}

static void on_frame_thumb_changed(OflFrame *fr, GdkTexture *tex, gpointer user_data) {
  (void)fr;
  GtkPicture *pic = GTK_PICTURE(user_data);
  gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
}

static void bind_list_item(GtkListItemFactory *factory, GtkListItem *item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *child = gtk_list_item_get_child(item);
  GtkPicture *pic = GTK_PICTURE(child);

  OflFrame *fr = gtk_list_item_get_item(item);
  if (!fr) return;

  // Listen to frame thumbnail changes so the picture updates immediately when conversion completes
  gulong handler_id = g_signal_connect(fr, "thumbnail-changed",
                                       G_CALLBACK(on_frame_thumb_changed), pic);
  g_object_set_data(G_OBJECT(item), "thumb_handler_id", GUINT_TO_POINTER(handler_id));

  GdkTexture *cached = ofl_frame_get_thumbnail(fr);
  if (cached) {
    gtk_picture_set_paintable(pic, GDK_PAINTABLE(cached));
    return;
  }

  GFile *file = ofl_frame_get_file(fr);
  if (!file) {
    gtk_picture_set_paintable(pic, NULL);
    return;
  }

  char *path = g_file_get_path(file);
  if (!path) {
    gtk_picture_set_paintable(pic, NULL);
    return;
  }

  GError *err = NULL;
  GdkTexture *tex = ofl_load_initial_thumbnail(path, &err);
  if (tex) {
    ofl_frame_set_thumbnail(fr, tex); // Emits signal, updating pic paintable
    g_object_unref(tex);
  } else {
    gtk_picture_set_paintable(pic, NULL);
    if (err) g_clear_error(&err);
  }

  g_free(path);
}

static void unbind_list_item(GtkListItemFactory *factory, GtkListItem *item, gpointer user_data) {
  (void)factory;
  (void)user_data;
  OflFrame *fr = gtk_list_item_get_item(item);
  gulong handler_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "thumb_handler_id"));
  if (fr && handler_id > 0) {
    g_signal_handler_disconnect(fr, handler_id);
  }
  g_object_set_data(G_OBJECT(item), "thumb_handler_id", NULL);
}

// ---------- Crop Tool & Overlay ----------
static GtkWidget* create_crop_icon_widget(void) {
  static const char crop_svg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">"
    "  <path fill=\"none\" stroke=\"#e6e6ec\" stroke-width=\"1.75\" stroke-linecap=\"round\" stroke-linejoin=\"round\" "
    "        d=\"M5 1v10h10 M1 5h10v10\" />"
    "</svg>";
  GBytes *b = g_bytes_new_static(crop_svg, sizeof(crop_svg) - 1);
  GdkTexture *tex = gdk_texture_new_from_bytes(b, NULL);
  g_bytes_unref(b);
  if (tex) {
    GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
    g_object_unref(tex);
    return img;
  }
  return gtk_label_new("⚲");
}

static gboolean get_displayed_image_rect(OflApp *s, double *out_x, double *out_y, double *out_w, double *out_h) {
  if (!s->preview || !s->last_img) return FALSE;
  GdkPaintable *paintable = gtk_picture_get_paintable(s->preview);
  if (!paintable) return FALSE;

  double img_w = gdk_paintable_get_intrinsic_width(paintable);
  double img_h = gdk_paintable_get_intrinsic_height(paintable);
  if (img_w <= 0 || img_h <= 0) return FALSE;

  int pw = gtk_widget_get_width(GTK_WIDGET(s->preview));
  int ph = gtk_widget_get_height(GTK_WIDGET(s->preview));
  if (pw <= 0 || ph <= 0) return FALSE;

  double img_ar = img_w / img_h;
  double win_ar = (double)pw / (double)ph;

  double draw_w, draw_h, off_x, off_y;
  if (win_ar > img_ar) {
    draw_h = (double)ph;
    draw_w = draw_h * img_ar;
    off_x = ((double)pw - draw_w) * 0.5;
    off_y = 0.0;
  } else {
    draw_w = (double)pw;
    draw_h = draw_w / img_ar;
    off_x = 0.0;
    off_y = ((double)ph - draw_h) * 0.5;
  }

  if (s->crop_overlay && gtk_widget_is_visible(s->crop_overlay)) {
    graphene_point_t p_in = GRAPHENE_POINT_INIT((float)off_x, (float)off_y);
    graphene_point_t p_out;
    if (gtk_widget_compute_point(GTK_WIDGET(s->preview), s->crop_overlay, &p_in, &p_out)) {
      off_x = p_out.x;
      off_y = p_out.y;
    }
  }

  if (out_x) *out_x = off_x;
  if (out_y) *out_y = off_y;
  if (out_w) *out_w = draw_w;
  if (out_h) *out_h = draw_h;
  return TRUE;
}

static int hit_test_crop(OflApp *s, double x, double y, const OflCrop *crop,
                         double off_x, double off_y, double draw_w, double draw_h) {
  (void)s;
  if (!crop) return CROP_HIT_NONE;

  double bx = off_x + crop->x * draw_w;
  double by = off_y + crop->y * draw_h;
  double bw = crop->width * draw_w;
  double bh = crop->height * draw_h;

  const double c_tol = 16.0; // Corner hit tolerance in pixels
  const double e_tol = 10.0; // Edge hit tolerance in pixels

  // Corners (prioritized over edges)
  if (fabs(x - bx) <= c_tol && fabs(y - by) <= c_tol) return CROP_HIT_TL;
  if (fabs(x - (bx + bw)) <= c_tol && fabs(y - by) <= c_tol) return CROP_HIT_TR;
  if (fabs(x - bx) <= c_tol && fabs(y - (by + bh)) <= c_tol) return CROP_HIT_BL;
  if (fabs(x - (bx + bw)) <= c_tol && fabs(y - (by + bh)) <= c_tol) return CROP_HIT_BR;

  // Edges
  if (fabs(y - by) <= e_tol && x >= bx - e_tol && x <= bx + bw + e_tol) return CROP_HIT_TOP;
  if (fabs(y - (by + bh)) <= e_tol && x >= bx - e_tol && x <= bx + bw + e_tol) return CROP_HIT_BOTTOM;
  if (fabs(x - bx) <= e_tol && y >= by - e_tol && y <= by + bh + e_tol) return CROP_HIT_LEFT;
  if (fabs(x - (bx + bw)) <= e_tol && y >= by - e_tol && y <= by + bh + e_tol) return CROP_HIT_RIGHT;

  // Inside
  if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) return CROP_HIT_INSIDE;

  return CROP_HIT_NONE;
}

static void update_crop_cursor(OflApp *s, int hit) {
  if (!s->crop_overlay) return;
  const char *cursor_name = NULL;
  switch (hit) {
    case CROP_HIT_TL:
    case CROP_HIT_BR:
      cursor_name = "nwse-resize";
      break;
    case CROP_HIT_TR:
    case CROP_HIT_BL:
      cursor_name = "nesw-resize";
      break;
    case CROP_HIT_TOP:
    case CROP_HIT_BOTTOM:
      cursor_name = "ns-resize";
      break;
    case CROP_HIT_LEFT:
    case CROP_HIT_RIGHT:
      cursor_name = "ew-resize";
      break;
    case CROP_HIT_INSIDE:
      cursor_name = "move";
      break;
    default:
      cursor_name = "default";
      break;
  }
  GdkCursor *c = gdk_cursor_new_from_name(cursor_name, NULL);
  gtk_widget_set_cursor(s->crop_overlay, c);
  if (c) g_object_unref(c);
}

static void on_crop_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
  (void)controller;
  OflApp *s = user_data;
  if (!s->crop_mode) return;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (!fr) return;

  const OflCrop *crop = ofl_frame_get_crop(fr);
  double off_x, off_y, draw_w, draw_h;
  if (crop && get_displayed_image_rect(s, &off_x, &off_y, &draw_w, &draw_h)) {
    int hit = hit_test_crop(s, x, y, crop, off_x, off_y, draw_w, draw_h);
    update_crop_cursor(s, hit);
  }
  g_object_unref(fr);
}

static void on_crop_motion_leave(GtkEventControllerMotion *controller, gpointer user_data) {
  (void)controller;
  OflApp *s = user_data;
  if (s->crop_overlay) {
    gtk_widget_set_cursor(s->crop_overlay, NULL);
  }
}

static void on_crop_drag_begin(GtkGestureDrag *gesture, double start_x, double start_y, gpointer user_data) {
  (void)gesture;
  OflApp *s = user_data;
  if (!s->crop_mode) return;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (!fr) return;

  const OflCrop *cr = ofl_frame_get_crop(fr);
  if (!cr) {
    g_object_unref(fr);
    return;
  }

  double off_x, off_y, draw_w, draw_h;
  if (!get_displayed_image_rect(s, &off_x, &off_y, &draw_w, &draw_h)) {
    g_object_unref(fr);
    return;
  }

  s->crop_hit = hit_test_crop(s, start_x, start_y, cr, off_x, off_y, draw_w, draw_h);
  s->crop_drag_start_x = start_x;
  s->crop_drag_start_y = start_y;
  s->crop_start_rect = *cr;
  g_object_unref(fr);
}

static void on_crop_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  (void)gesture;
  OflApp *s = user_data;
  if (!s->crop_mode || s->crop_hit == CROP_HIT_NONE) return;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (!fr) return;

  double off_x, off_y, draw_w, draw_h;
  if (!get_displayed_image_rect(s, &off_x, &off_y, &draw_w, &draw_h) || draw_w <= 0 || draw_h <= 0) {
    g_object_unref(fr);
    return;
  }

  double d_nx = offset_x / draw_w;
  double d_ny = offset_y / draw_h;

  OflCrop new_cr = s->crop_start_rect;
  new_cr.enabled = TRUE;

  const float min_sz = 0.02f; // Minimum crop dimension (normalized)

  switch (s->crop_hit) {
    case CROP_HIT_INSIDE: {
      float nx = s->crop_start_rect.x + (float)d_nx;
      float ny = s->crop_start_rect.y + (float)d_ny;
      nx = CLAMP(nx, 0.0f, 1.0f - new_cr.width);
      ny = CLAMP(ny, 0.0f, 1.0f - new_cr.height);
      new_cr.x = nx;
      new_cr.y = ny;
      break;
    }
    case CROP_HIT_TL: {
      float r = s->crop_start_rect.x + s->crop_start_rect.width;
      float b = s->crop_start_rect.y + s->crop_start_rect.height;
      float nx = CLAMP(s->crop_start_rect.x + (float)d_nx, 0.0f, r - min_sz);
      float ny = CLAMP(s->crop_start_rect.y + (float)d_ny, 0.0f, b - min_sz);
      new_cr.x = nx;
      new_cr.y = ny;
      new_cr.width = r - nx;
      new_cr.height = b - ny;
      break;
    }
    case CROP_HIT_TR: {
      float l = s->crop_start_rect.x;
      float b = s->crop_start_rect.y + s->crop_start_rect.height;
      float nr = CLAMP(s->crop_start_rect.x + s->crop_start_rect.width + (float)d_nx, l + min_sz, 1.0f);
      float ny = CLAMP(s->crop_start_rect.y + (float)d_ny, 0.0f, b - min_sz);
      new_cr.x = l;
      new_cr.y = ny;
      new_cr.width = nr - l;
      new_cr.height = b - ny;
      break;
    }
    case CROP_HIT_BL: {
      float r = s->crop_start_rect.x + s->crop_start_rect.width;
      float t = s->crop_start_rect.y;
      float nx = CLAMP(s->crop_start_rect.x + (float)d_nx, 0.0f, r - min_sz);
      float nb = CLAMP(s->crop_start_rect.y + s->crop_start_rect.height + (float)d_ny, t + min_sz, 1.0f);
      new_cr.x = nx;
      new_cr.y = t;
      new_cr.width = r - nx;
      new_cr.height = nb - t;
      break;
    }
    case CROP_HIT_BR: {
      float l = s->crop_start_rect.x;
      float t = s->crop_start_rect.y;
      float nr = CLAMP(s->crop_start_rect.x + s->crop_start_rect.width + (float)d_nx, l + min_sz, 1.0f);
      float nb = CLAMP(s->crop_start_rect.y + s->crop_start_rect.height + (float)d_ny, t + min_sz, 1.0f);
      new_cr.x = l;
      new_cr.y = t;
      new_cr.width = nr - l;
      new_cr.height = nb - t;
      break;
    }
    case CROP_HIT_TOP: {
      float b = s->crop_start_rect.y + s->crop_start_rect.height;
      float ny = CLAMP(s->crop_start_rect.y + (float)d_ny, 0.0f, b - min_sz);
      new_cr.y = ny;
      new_cr.height = b - ny;
      break;
    }
    case CROP_HIT_BOTTOM: {
      float t = s->crop_start_rect.y;
      float nb = CLAMP(s->crop_start_rect.y + s->crop_start_rect.height + (float)d_ny, t + min_sz, 1.0f);
      new_cr.height = nb - t;
      break;
    }
    case CROP_HIT_LEFT: {
      float r = s->crop_start_rect.x + s->crop_start_rect.width;
      float nx = CLAMP(s->crop_start_rect.x + (float)d_nx, 0.0f, r - min_sz);
      new_cr.x = nx;
      new_cr.width = r - nx;
      break;
    }
    case CROP_HIT_RIGHT: {
      float l = s->crop_start_rect.x;
      float nr = CLAMP(s->crop_start_rect.x + s->crop_start_rect.width + (float)d_nx, l + min_sz, 1.0f);
      new_cr.width = nr - l;
      break;
    }
    default:
      break;
  }

  ofl_frame_set_crop(fr, &new_cr);
  g_object_unref(fr);

  if (s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
}

static void on_crop_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data) {
  (void)gesture;
  (void)offset_x;
  (void)offset_y;
  OflApp *s = user_data;
  s->crop_hit = CROP_HIT_NONE;
}

static void draw_crop_overlay_cb(GtkDrawingArea *drawing_area, cairo_t *cr,
                                 int width, int height, gpointer user_data) {
  (void)drawing_area;
  (void)width;
  (void)height;
  OflApp *s = user_data;
  if (!s->crop_mode) return;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (!fr) return;

  const OflCrop *crop = ofl_frame_get_crop(fr);
  if (!crop) {
    g_object_unref(fr);
    return;
  }

  double off_x, off_y, draw_w, draw_h;
  if (!get_displayed_image_rect(s, &off_x, &off_y, &draw_w, &draw_h)) {
    g_object_unref(fr);
    return;
  }

  double bx = off_x + crop->x * draw_w;
  double by = off_y + crop->y * draw_h;
  double bw = crop->width * draw_w;
  double bh = crop->height * draw_h;

  // 1. Dim mask outside crop area
  cairo_save(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cr, off_x, off_y, draw_w, draw_h);
  cairo_rectangle(cr, bx, by, bw, bh);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.60);
  cairo_fill(cr);
  cairo_restore(cr);

  // 2. Rule-of-thirds grid
  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30);
  cairo_set_line_width(cr, 1.0);
  double x1 = bx + bw / 3.0;
  double x2 = bx + 2.0 * bw / 3.0;
  double y1 = by + bh / 3.0;
  double y2 = by + 2.0 * bh / 3.0;
  cairo_move_to(cr, x1, by); cairo_line_to(cr, x1, by + bh);
  cairo_move_to(cr, x2, by); cairo_line_to(cr, x2, by + bh);
  cairo_move_to(cr, bx, y1); cairo_line_to(cr, bx + bw, y1);
  cairo_move_to(cr, bx, y2); cairo_line_to(cr, bx + bw, y2);
  cairo_stroke(cr);
  cairo_restore(cr);

  // 3. Crop border (dark contrast outline + crisp white border)
  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, 2.5);
  cairo_rectangle(cr, bx, by, bw, bh);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
  cairo_set_line_width(cr, 1.5);
  cairo_rectangle(cr, bx, by, bw, bh);
  cairo_stroke(cr);
  cairo_restore(cr);

  // 4. Corner L-brackets & Edge handles
  cairo_save(cr);
  double h_len = 16.0;
  if (h_len > bw * 0.4) h_len = bw * 0.4;
  if (h_len > bh * 0.4) h_len = bh * 0.4;

  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0) {
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
      cairo_set_line_width(cr, 4.5);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    } else {
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
      cairo_set_line_width(cr, 2.5);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    }
    // Top-Left corner
    cairo_move_to(cr, bx, by + h_len);
    cairo_line_to(cr, bx, by);
    cairo_line_to(cr, bx + h_len, by);

    // Top-Right corner
    cairo_move_to(cr, bx + bw - h_len, by);
    cairo_line_to(cr, bx + bw, by);
    cairo_line_to(cr, bx + bw, by + h_len);

    // Bottom-Left corner
    cairo_move_to(cr, bx, by + bh - h_len);
    cairo_line_to(cr, bx, by + bh);
    cairo_line_to(cr, bx + h_len, by + bh);

    // Bottom-Right corner
    cairo_move_to(cr, bx + bw - h_len, by + bh);
    cairo_line_to(cr, bx + bw, by + bh);
    cairo_line_to(cr, bx + bw, by + bh - h_len);

    // Center edge handles
    double e_bar = 14.0;
    // Top center
    cairo_move_to(cr, bx + bw * 0.5 - e_bar, by);
    cairo_line_to(cr, bx + bw * 0.5 + e_bar, by);
    // Bottom center
    cairo_move_to(cr, bx + bw * 0.5 - e_bar, by + bh);
    cairo_line_to(cr, bx + bw * 0.5 + e_bar, by + bh);
    // Left center
    cairo_move_to(cr, bx, by + bh * 0.5 - e_bar);
    cairo_line_to(cr, bx, by + bh * 0.5 + e_bar);
    // Right center
    cairo_move_to(cr, bx + bw, by + bh * 0.5 - e_bar);
    cairo_line_to(cr, bx + bw, by + bh * 0.5 + e_bar);

    cairo_stroke(cr);
  }
  cairo_restore(cr);

  // 5. Resolution Info Pill (Real-time pixel width & height)
  int fr_full_w = 0, fr_full_h = 0;
  ofl_frame_get_full_size(fr, &fr_full_w, &fr_full_h);
  int rot = ofl_frame_get_rotate_steps(fr);
  rot = ((rot % 4) + 4) % 4;
  if (fr_full_w <= 0 || fr_full_h <= 0) {
    if (s->last_img) {
      fr_full_w = (s->last_img->full_width > 0) ? s->last_img->full_width : s->last_img->width;
      fr_full_h = (s->last_img->full_height > 0) ? s->last_img->full_height : s->last_img->height;
    }
  }
  int base_w = (rot % 2 == 0) ? fr_full_w : fr_full_h;
  int base_h = (rot % 2 == 0) ? fr_full_h : fr_full_w;

  int crop_px_w = (int)round(crop->width * base_w);
  int crop_px_h = (int)round(crop->height * base_h);

  char res_text[64];
  snprintf(res_text, sizeof(res_text), "%d × %d px", crop_px_w, crop_px_h);

  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font_desc = pango_font_description_from_string("SF Pro Text, -apple-system, Cantarell, Sans 11");
  pango_layout_set_font_description(layout, font_desc);
  pango_font_description_free(font_desc);
  pango_layout_set_text(layout, res_text, -1);

  int text_w = 0, text_h = 0;
  pango_layout_get_pixel_size(layout, &text_w, &text_h);

  double pad_x = 10.0;
  double pad_y = 5.0;
  double pill_w = text_w + pad_x * 2.0;
  double pill_h = text_h + pad_y * 2.0;

  double pill_x = bx + (bw - pill_w) * 0.5;
  double pill_y = by + bh + 12.0; // Float below crop area
  if (pill_y + pill_h > off_y + draw_h - 6.0) {
    pill_y = by + bh - pill_h - 12.0; // Float inside bottom if near boundary
  }
  if (pill_y < by + 6.0) {
    pill_y = by + 12.0; // Float inside top if area is short
  }

  cairo_save(cr);
  double rad = 6.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, pill_x + pill_w - rad, pill_y + rad, rad, -G_PI_2, 0);
  cairo_arc(cr, pill_x + pill_w - rad, pill_y + pill_h - rad, rad, 0, G_PI_2);
  cairo_arc(cr, pill_x + rad, pill_y + pill_h - rad, rad, G_PI_2, G_PI);
  cairo_arc(cr, pill_x + rad, pill_y + rad, rad, G_PI, 3.0 * G_PI_2);
  cairo_close_path(cr);

  cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.88);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 1.0);
  cairo_move_to(cr, pill_x + pad_x, pill_y + pad_y);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  g_object_unref(layout);
  g_object_unref(fr);
}

static void on_crop_tool_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;

  if (s->crop_mode) {
    // Exit crop mode
    s->crop_mode = FALSE;
    if (s->btn_crop) {
      gtk_widget_remove_css_class(GTK_WIDGET(s->btn_crop), "suggested-action");
    }
    if (s->crop_overlay) {
      gtk_widget_set_visible(s->crop_overlay, FALSE);
    }
    render_selected_preview(s);
    return;
  }

  // Enter crop mode
  if (s->pick_base_mode) {
    s->pick_base_mode = FALSE;
    gtk_widget_set_cursor(GTK_WIDGET(s->preview), NULL);
    gtk_button_set_label(s->btn_pick, "Pick Film Base");
    gtk_widget_remove_css_class(GTK_WIDGET(s->btn_pick), "suggested-action");
    gtk_widget_set_visible(s->hud_box, FALSE);
  }

  s->crop_mode = TRUE;
  if (s->btn_crop) {
    gtk_widget_add_css_class(GTK_WIDGET(s->btn_crop), "suggested-action");
  }

  // Ensure active frame has a crop initialized
  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (fr) {
    const OflCrop *cr = ofl_frame_get_crop(fr);
    if (!cr || !cr->enabled) {
      OflCrop def_cr;
      ofl_crop_init_defaults(&def_cr);
      def_cr.enabled = TRUE;
      ofl_frame_set_crop(fr, &def_cr);
    }
    g_object_unref(fr);
  }

  render_selected_preview(s);

  if (s->crop_overlay) {
    gtk_widget_set_visible(s->crop_overlay, TRUE);
    gtk_widget_queue_draw(s->crop_overlay);
  }
}

static void on_reset_crop_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  OflApp *s = user_data;

  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), s->active_frame_idx);
  if (!fr) return;

  OflCrop def_cr;
  ofl_crop_init_defaults(&def_cr);
  if (s->crop_mode) {
    def_cr.enabled = TRUE;
  } else {
    def_cr.enabled = FALSE;
  }
  ofl_frame_set_crop(fr, &def_cr);
  g_object_unref(fr);

  if (s->crop_mode && s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
  render_selected_preview(s);
}

static guint8* crop_rgba8(guint8 *rgba, int *in_w, int *in_h, int *in_stride, const OflCrop *cr) {
  if (!rgba || !in_w || !in_h || !in_stride || !cr || !cr->enabled) return rgba;
  int w = *in_w;
  int h = *in_h;
  int stride = *in_stride;
  if (w <= 0 || h <= 0 || stride <= 0) return rgba;

  int cx = (int)roundf(cr->x * (float)w);
  int cy = (int)roundf(cr->y * (float)h);
  int cw = (int)roundf(cr->width * (float)w);
  int ch = (int)roundf(cr->height * (float)h);

  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx + cw > w) cw = w - cx;
  if (cy + ch > h) ch = h - cy;

  if (cw <= 10 || ch <= 10 || (cw == w && ch == h)) {
    return rgba;
  }

  guint8 *cropped = g_malloc((size_t)cw * 4 * (size_t)ch);
  for (int y = 0; y < ch; y++) {
    const guint8 *s_row = rgba + ((size_t)(cy + y) * (size_t)stride) + ((size_t)cx * 4);
    guint8 *d_row = cropped + ((size_t)y * (size_t)cw * 4);
    memcpy(d_row, s_row, (size_t)cw * 4);
  }
  g_free(rgba);

  *in_w = cw;
  *in_h = ch;
  *in_stride = cw * 4;
  return cropped;
}

// ---------- Core render ----------
static void render_selected_preview(OflApp *s) {
  guint n_frames = s->frames ? g_list_model_get_n_items(G_LIST_MODEL(s->frames)) : 0;
  if (n_frames == 0 || s->active_frame_idx >= n_frames) return;
  guint idx = s->active_frame_idx;

  OflFrame *fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!fr) return;

  GFile *file = ofl_frame_get_file(fr);
  if (!file) {
    g_object_unref(fr);
    return;
  }

  char *path = g_file_get_path(file);
  int rot = ofl_frame_get_rotate_steps(fr);
  OflAdjustments adj = *ofl_frame_get_adjustments(fr);

  g_object_unref(fr);

  if (!path) return;

  // Only decode image from disk if selection changed
  if (!s->last_img || !s->last_img_path || g_strcmp0(s->last_img_path, path) != 0) {
    GError *err = NULL;
    OflRgb16 *img = ofl_load_image_rgb16(path, &err);
    if (!img) {
      if (err) { g_warning("image decode failed: %s", err->message); g_clear_error(&err); }
      g_free(path);
      return;
    }

    if (s->last_img) ofl_rgb16_free(s->last_img);
    s->last_img = img;
    g_free(s->last_img_path);
    s->last_img_path = g_strdup(path);
  }
  g_free(path);

  // Store full sensor dimensions on frame for precise crop pixel readouts
  if (s->last_img) {
    OflFrame *info_fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
    if (info_fr) {
      int fw = (s->last_img->full_width > 0) ? s->last_img->full_width : s->last_img->width;
      int fh = (s->last_img->full_height > 0) ? s->last_img->full_height : s->last_img->height;
      ofl_frame_set_full_size(info_fr, fw, fh);
      g_object_unref(info_fr);
    }
  }

  // If no roll base selected or currently picking base, show RAW negative preview
  if (!s->has_roll_base || s->pick_base_mode) {
    int out_w = 0, out_h = 0, stride = 0;
    guint8 *rgba = ofl_raw_preview_to_rgba8(s->last_img->rgb,
                                           s->last_img->width,
                                           s->last_img->height,
                                           rot,
                                           &adj,
                                           &s->preview_white_pt,
                                           &out_w, &out_h, &stride);
    if (!rgba || out_w <= 0 || out_h <= 0 || stride <= 0) return;

    OflFrame *cur_fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
    if (!s->crop_mode && cur_fr) {
      const OflCrop *cr = ofl_frame_get_crop(cur_fr);
      rgba = crop_rgba8(rgba, &out_w, &out_h, &stride, cr);
    }

    // If frame is rotated or adjusted before conversion, update its thumbnail to match
    if (rot != 0 || adj.temperature != 0.0f || adj.tint != 0.0f ||
        adj.exposure != 0.0f || adj.contrast != 0.0f || adj.brightness != 0.0f) {
      if (cur_fr) {
        int tw, th, t_stride;
        guint8 *t_buf = create_thumb_pixels_rgba8(rgba, out_w, out_h, stride, 160, &tw, &th, &t_stride);
        if (t_buf) {
          GBytes *t_bytes = g_bytes_new_take(t_buf, (size_t)t_stride * (size_t)th);
          GdkTexture *t_tex = gdk_memory_texture_new(tw, th, GDK_MEMORY_R8G8B8A8, t_bytes, (gsize)t_stride);
          g_bytes_unref(t_bytes);
          if (t_tex) {
            ofl_frame_set_thumbnail(cur_fr, t_tex);
            g_object_unref(t_tex);
          }
        }
      }
    }
    if (cur_fr) g_object_unref(cur_fr);

    GBytes *bytes = g_bytes_new_take(rgba, (gsize)stride * (gsize)out_h);
    GdkTexture *tex = gdk_memory_texture_new(out_w, out_h,
                                             GDK_MEMORY_R8G8B8A8,
                                             bytes,
                                             (gsize)stride);
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(s->preview, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    if (s->crop_mode && s->crop_overlay) {
      gtk_widget_queue_draw(s->crop_overlay);
    }
    return;
  }

  // Converted preview (negative -> positive)
  OflNegParams p = s->neg;
  p.rotate_steps = rot;
  p.base_rgb[0] = s->roll_base_rgb[0];
  p.base_rgb[1] = s->roll_base_rgb[1];
  p.base_rgb[2] = s->roll_base_rgb[2];
  p.temperature = adj.temperature;
  p.tint = adj.tint;
  p.exposure = adj.exposure;
  p.contrast = adj.contrast;
  p.brightness = adj.brightness;

  int out_w=0, out_h=0, stride=0;
  guint8 *rgba = ofl_neg_convert_to_rgba8(s->last_img->rgb,
                                         s->last_img->width,
                                         s->last_img->height,
                                         &p, &out_w, &out_h, &stride);

  // Ensure RGBA8 layout and stride
  rgba = ensure_rgba8(rgba, out_w, out_h, &stride);
  if (!rgba || out_w <= 0 || out_h <= 0 || stride <= 0) return;

  OflFrame *cur_fr = g_list_model_get_item(G_LIST_MODEL(s->frames), idx);
  if (!s->crop_mode && cur_fr) {
    const OflCrop *cr = ofl_frame_get_crop(cur_fr);
    rgba = crop_rgba8(rgba, &out_w, &out_h, &stride, cr);
  }

  // Immediately update current frame's thumbnail to positive!
  if (cur_fr) {
    int tw, th, t_stride;
    guint8 *t_buf = create_thumb_pixels_rgba8(rgba, out_w, out_h, stride, 160, &tw, &th, &t_stride);
    if (t_buf) {
      GBytes *t_bytes = g_bytes_new_take(t_buf, (size_t)t_stride * (size_t)th);
      GdkTexture *t_tex = gdk_memory_texture_new(tw, th, GDK_MEMORY_R8G8B8A8, t_bytes, (gsize)t_stride);
      g_bytes_unref(t_bytes);
      if (t_tex) {
        ofl_frame_set_thumbnail(cur_fr, t_tex);
        g_object_unref(t_tex);
      }
    }
    g_object_unref(cur_fr);
  }

  GBytes *bytes = g_bytes_new_take(rgba, (gsize)stride * (gsize)out_h);
  GdkTexture *tex = gdk_memory_texture_new(out_w, out_h,
                                           GDK_MEMORY_R8G8B8A8,
                                           bytes,
                                           (gsize)stride);
  g_bytes_unref(bytes);
  gtk_picture_set_paintable(s->preview, GDK_PAINTABLE(tex));
  g_object_unref(tex);

  if (s->crop_mode && s->crop_overlay) {
    gtk_widget_queue_draw(s->crop_overlay);
  }
}

// ---------- App wiring ----------
static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  OflApp *s = g_new0(OflApp, 1);
  s->app = app;
  s->filmstrip_grid = NULL;

  s->last_img = NULL;
  s->last_img_path = NULL;
  s->pick_base_mode = FALSE;
  s->has_picked_base = FALSE;
  s->has_roll_base = FALSE;
  s->roll_base_rgb[0] = OFL_DEFAULT_BASE_R;
  s->roll_base_rgb[1] = OFL_DEFAULT_BASE_G;
  s->roll_base_rgb[2] = OFL_DEFAULT_BASE_B;
  s->hover_rgb[0] = OFL_DEFAULT_BASE_R;
  s->hover_rgb[1] = OFL_DEFAULT_BASE_G;
  s->hover_rgb[2] = OFL_DEFAULT_BASE_B;
  s->display_rgb[0] = (float)OFL_DEFAULT_BASE_8BIT_R / 255.0f;
  s->display_rgb[1] = (float)OFL_DEFAULT_BASE_8BIT_G / 255.0f;
  s->display_rgb[2] = (float)OFL_DEFAULT_BASE_8BIT_B / 255.0f;
  s->preview_white_pt = 65535;
  s->roll_epoch = 0;

  // Neg defaults
  s->neg.base_rgb[0] = OFL_DEFAULT_BASE_R;
  s->neg.base_rgb[1] = OFL_DEFAULT_BASE_G;
  s->neg.base_rgb[2] = OFL_DEFAULT_BASE_B;
  s->neg.gamma = 1.0f/2.2f;
  s->neg.rotate_steps = 0;

  // Prefer dark theme across the entire application and ensure minimize, maximize, close buttons
  GtkSettings *settings = gtk_settings_get_default();
  if (settings) {
    g_object_set(settings,
                 "gtk-application-prefer-dark-theme", TRUE,
                 "gtk-decoration-layout", ":minimize,maximize,close",
                 NULL);
  }

  s->win = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_title(s->win, "Open Film Lab");
  gtk_window_set_default_size(s->win, 1200, 800);
  gtk_widget_add_css_class(GTK_WIDGET(s->win), "dark-window");

  // Setup application icon (macOS Dock, App Switcher, window)
  ofl_setup_app_icon(s->win);

  // Comprehensive Dark Mode CSS
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css,
    "window, .dark-window {"
    "  background-color: #121215;"
    "  color: #e6e6ec;"
    "}"
    "headerbar {"
    "  background-color: #19191d;"
    "  border-bottom: 1px solid rgba(255, 255, 255, 0.09);"
    "  color: #f0f0f4;"
    "}"
    "headerbar windowhandle {"
    "  background-color: transparent;"
    "}"
    "headerbar button {"
    "  background-color: #24242a;"
    "  color: #e6e6ec;"
    "  border: 1px solid rgba(255, 255, 255, 0.12);"
    "  border-radius: 6px;"
    "}"
    "headerbar button:hover {"
    "  background-color: #303038;"
    "  color: #ffffff;"
    "}"
    "headerbar button.suggested-action {"
    "  background-color: #2d6cdf;"
    "  color: #ffffff;"
    "  border-color: #4382f6;"
    "}"
    "headerbar button.suggested-action:hover {"
    "  background-color: #3b7ced;"
    "}"
    ".preview-canvas {"
    "  background-color: #0c0c0e;"
    "  border: none;"
    "}"
    ".preview-canvas frame {"
    "  border: none;"
    "}"
    ".filmstrip-scroller {"
    "  background-color: #151518;"
    "  border-top: 1px solid rgba(255, 255, 255, 0.08);"
    "}"
    ".filmstrip-grid {"
    "  background-color: transparent;"
    "  padding: 8px 10px;"
    "}"
    "gridview > child {"
    "  padding: 3px;"
    "  border-radius: 6px;"
    "  transition: all 120ms ease-in-out;"
    "}"
    "gridview > child:hover {"
    "  background-color: rgba(255, 255, 255, 0.06);"
    "}"
    "gridview > child:selected {"
    "  background-color: #2655a6;"
    "  border-radius: 6px;"
    "}"
    "separator {"
    "  background-color: rgba(255, 255, 255, 0.08);"
    "}"
    "scale trough {"
    "  background-color: #2a2a32;"
    "  border-radius: 4px;"
    "}"
    "scale highlight {"
    "  background-color: #3b82f6;"
    "  border-radius: 4px;"
    "}"
    "scale slider {"
    "  background-color: #e5e5eb;"
    "  border-radius: 50%;"
    "}"
    "scale slider:hover {"
    "  background-color: #ffffff;"
    "}"
    ".hud-pill {"
    "  background-color: rgba(18, 18, 22, 0.88);"
    "  border: 1px solid rgba(255, 255, 255, 0.22);"
    "  border-radius: 20px;"
    "  padding: 6px 16px;"
    "  box-shadow: 0 4px 14px rgba(0, 0, 0, 0.45);"
    "}"
    ".hud-label {"
    "  color: #f5f5f7;"
    "  font-family: monospace, ui-monospace, Menlo, Consolas;"
    "  font-weight: 600;"
    "  font-size: 13px;"
    "}"
    ".adjustment-sidebar {"
    "  background-color: #17171a;"
    "  border-left: 1px solid rgba(255, 255, 255, 0.08);"
    "  min-width: 280px;"
    "}"
    ".adjustment-panel {"
    "  background-color: transparent;"
    "}"
    ".adj-title {"
    "  font-weight: 700;"
    "  font-size: 14px;"
    "  color: #f0f0f4;"
    "}"
    ".adj-section-header {"
    "  font-size: 11px;"
    "  font-weight: 700;"
    "  color: #828292;"
    "  letter-spacing: 0.6px;"
    "  margin-top: 4px;"
    "}"
    ".adj-control-label {"
    "  font-size: 12px;"
    "  font-weight: 500;"
    "  color: #d6d6dc;"
    "}"
    ".adj-value-label, .adj-value-entry {"
    "  font-family: monospace, ui-monospace, Menlo, Consolas;"
    "  font-size: 11px;"
    "  font-weight: 600;"
    "  color: #5c9df5;"
    "  background-color: #17171b;"
    "  border: 1px solid rgba(255, 255, 255, 0.14);"
    "  border-radius: 4px;"
    "  padding: 1px 4px;"
    "  min-height: 20px;"
    "  min-width: 52px;"
    "  margin: 0;"
    "  transition: border-color 0.15s ease, background-color 0.15s ease;"
    "}"
    ".adj-value-entry:focus {"
    "  border-color: #5c9df5;"
    "  background-color: #202028;"
    "  color: #ffffff;"
    "  outline: none;"
    "  box-shadow: 0 0 0 1px #5c9df5;"
    "}"
    ".adj-value-entry.invalid-input {"
    "  border-color: #f85149;"
    "  color: #f85149;"
    "  box-shadow: 0 0 0 1px #f85149;"
    "}"
    ".adj-reset-btn {"
    "  padding: 1px 5px;"
    "  font-size: 10px;"
    "  min-height: 18px;"
    "  min-width: 18px;"
    "  border-radius: 4px;"
    "  opacity: 0.75;"
    "}"
    ".adj-reset-btn:hover {"
    "  opacity: 1.0;"
    "}"
    ".adj-reset-all-btn {"
    "  padding: 3px 8px;"
    "  font-size: 11px;"
    "  font-weight: 600;"
    "  border-radius: 5px;"
    "}"
    ".export-card {"
    "  background-color: #1a1a1e;"
    "  border: 1px solid rgba(255, 255, 255, 0.08);"
    "  border-radius: 8px;"
    "  padding: 12px 14px;"
    "}"
    ".export-section-title {"
    "  font-size: 11px;"
    "  font-weight: 700;"
    "  color: #8a8a9a;"
    "  letter-spacing: 0.6px;"
    "}"
    ".token-chip {"
    "  padding: 2px 8px;"
    "  font-size: 11px;"
    "  font-family: monospace, ui-monospace, Menlo, Consolas;"
    "  border-radius: 12px;"
    "  background-color: #24242c;"
    "  color: #5c9df5;"
    "  border: 1px solid rgba(92, 157, 245, 0.25);"
    "}"
    ".token-chip:hover {"
    "  background-color: #2e354a;"
    "  color: #85b7ff;"
    "}");
  gtk_style_context_add_provider_for_display(
    gdk_display_get_default(),
    GTK_STYLE_PROVIDER(css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  // Model
  s->frames = g_list_store_new(OFL_TYPE_FRAME);
  s->sel = gtk_multi_selection_new(G_LIST_MODEL(s->frames));
  g_signal_connect(s->sel, "selection-changed", G_CALLBACK(on_selection_model_selection_changed), s);

  // Root layout
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(s->win, root);

  // Headerbar
  GtkWidget *hb = gtk_header_bar_new();
  gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb), ":minimize,maximize,close");
  gtk_window_set_titlebar(s->win, hb);

  // Left: Open folder + Export button
  GtkWidget *btn = gtk_button_new_with_label("Open Roll Folder");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_open_folder_clicked), s);

  GtkWidget *btn_exp = gtk_button_new_with_label("Export…");
  s->btn_export = GTK_BUTTON(btn_exp);
  gtk_widget_set_sensitive(btn_exp, FALSE);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), btn_exp);
  g_signal_connect(btn_exp, "clicked", G_CALLBACK(on_export_clicked), s);

  // Right: Rotate + Base pick/apply + Crop
  GtkWidget *btn_crop = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(btn_crop), create_crop_icon_widget());
  gtk_widget_set_tooltip_text(btn_crop, "Crop & Composition Tool");
  s->btn_crop = GTK_BUTTON(btn_crop);
  gtk_widget_set_sensitive(btn_crop, FALSE);

  GtkWidget *btn_ccw  = gtk_button_new_with_label("⟲");
  GtkWidget *btn_cw   = gtk_button_new_with_label("⟳");
  GtkWidget *btn_pick = gtk_button_new_with_label("Pick Film Base");
  GtkWidget *btn_apply= gtk_button_new_with_label("Apply to Roll");

  s->btn_pick = GTK_BUTTON(btn_pick);
  s->btn_apply = GTK_BUTTON(btn_apply);

  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_crop);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_cw);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_ccw);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_apply);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), btn_pick);

  g_signal_connect(btn_crop, "clicked", G_CALLBACK(on_crop_tool_clicked), s);
  g_signal_connect(btn_ccw,  "clicked", G_CALLBACK(on_rotate_ccw_clicked), s);
  g_signal_connect(btn_cw,   "clicked", G_CALLBACK(on_rotate_cw_clicked), s);
  g_signal_connect(btn_pick, "clicked", G_CALLBACK(on_pick_base_clicked), s);
  g_signal_connect(btn_apply,"clicked", G_CALLBACK(on_apply_roll_clicked), s);

  // Preview area
  s->preview = GTK_PICTURE(gtk_picture_new());
  gtk_picture_set_can_shrink(s->preview, TRUE);
  gtk_picture_set_content_fit(s->preview, GTK_CONTENT_FIT_CONTAIN);

  // Click controller for base picking
  GtkGesture *click = gtk_gesture_click_new();
  gtk_widget_add_controller(GTK_WIDGET(s->preview), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(click, "pressed", G_CALLBACK(on_preview_pressed), s);

  // Motion controller for real-time RGB hover feedback
  GtkEventController *motion = gtk_event_controller_motion_new();
  gtk_widget_add_controller(GTK_WIDGET(s->preview), motion);
  g_signal_connect(motion, "motion", G_CALLBACK(on_preview_motion), s);

  GtkWidget *preview_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(preview_frame, "preview-canvas");
  gtk_frame_set_child(GTK_FRAME(preview_frame), GTK_WIDGET(s->preview));
  gtk_widget_set_hexpand(preview_frame, TRUE);
  gtk_widget_set_vexpand(preview_frame, TRUE);

  // Wrap preview in GtkOverlay for HUD badge and Crop overlay
  GtkWidget *overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_vexpand(overlay, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), preview_frame);

  // Crop overlay drawing area
  s->crop_overlay = gtk_drawing_area_new();
  gtk_widget_set_hexpand(s->crop_overlay, TRUE);
  gtk_widget_set_vexpand(s->crop_overlay, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->crop_overlay), draw_crop_overlay_cb, s, NULL);

  GtkGesture *crop_drag = gtk_gesture_drag_new();
  g_signal_connect(crop_drag, "drag-begin", G_CALLBACK(on_crop_drag_begin), s);
  g_signal_connect(crop_drag, "drag-update", G_CALLBACK(on_crop_drag_update), s);
  g_signal_connect(crop_drag, "drag-end", G_CALLBACK(on_crop_drag_end), s);
  gtk_widget_add_controller(s->crop_overlay, GTK_EVENT_CONTROLLER(crop_drag));

  GtkEventController *crop_motion = gtk_event_controller_motion_new();
  g_signal_connect(crop_motion, "motion", G_CALLBACK(on_crop_motion), s);
  g_signal_connect(crop_motion, "leave", G_CALLBACK(on_crop_motion_leave), s);
  gtk_widget_add_controller(s->crop_overlay, crop_motion);

  gtk_widget_set_visible(s->crop_overlay, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), s->crop_overlay);

  // Floating HUD pill
  s->hud_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(s->hud_box, "hud-pill");
  gtk_widget_set_halign(s->hud_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(s->hud_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(s->hud_box, 14);

  // Circular swatch
  s->hud_swatch = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(s->hud_swatch), 18);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(s->hud_swatch), 18);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->hud_swatch), draw_swatch_cb, s, NULL);
  gtk_box_append(GTK_BOX(s->hud_box), s->hud_swatch);

  // Label
  s->hud_label = gtk_label_new("Click 'Pick Film Base' to sample mask");
  gtk_widget_add_css_class(s->hud_label, "hud-label");
  gtk_box_append(GTK_BOX(s->hud_box), s->hud_label);

  gtk_widget_set_visible(s->hud_box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), s->hud_box);

  // Main horizontal layout (Preview canvas on left, Adjustment panel on right)
  GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(main_hbox, TRUE);
  gtk_widget_set_vexpand(main_hbox, TRUE);

  gtk_box_append(GTK_BOX(main_hbox), overlay);

  GtkWidget *sep_v = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_box_append(GTK_BOX(main_hbox), sep_v);

  GtkWidget *adj_panel = create_adjustments_panel(s);
  gtk_widget_set_hexpand(adj_panel, FALSE);
  gtk_box_append(GTK_BOX(main_hbox), adj_panel);

  gtk_box_append(GTK_BOX(root), main_hbox);

  // Filmstrip (GridView forced to single row)
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(setup_list_item), s);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_list_item), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(unbind_list_item), NULL);

  GtkGridView *grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(s->sel), factory));
  s->filmstrip_grid = grid;
  gtk_widget_add_css_class(GTK_WIDGET(grid), "filmstrip-grid");

  gtk_grid_view_set_min_columns(grid, 1);
  gtk_grid_view_set_max_columns(grid, 1);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_add_css_class(scroller, "filmstrip-scroller");
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(grid));
  gtk_widget_set_size_request(scroller, -1, 140);

  gtk_box_append(GTK_BOX(root), scroller);

  gtk_window_present(s->win);
}

int ofl_app_run(int argc, char **argv) {
  GtkApplication *app = gtk_application_new("lab.openfilm.openfilmlab", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}