#include "ofl_frame.h"

struct _OflFrame {
  GObject parent_instance;
  GFile *file;
  int rotate_steps;
  GdkTexture *thumbnail;
  OflAdjustments adjustments;
  OflCrop crop;
  int full_w;
  int full_h;
};

G_DEFINE_TYPE(OflFrame, ofl_frame, G_TYPE_OBJECT)

void ofl_adjustments_init_defaults(OflAdjustments *adj) {
  if (!adj) return;
  adj->temperature = 0.0f;
  adj->tint = 0.0f;
  adj->exposure = 0.0f;
  adj->contrast = 0.0f;
  adj->brightness = 0.0f;
}

void ofl_crop_init_defaults(OflCrop *crop) {
  if (!crop) return;
  crop->enabled = FALSE;
  crop->x = 0.05f;
  crop->y = 0.05f;
  crop->width = 0.90f;
  crop->height = 0.90f;
}

static void ofl_frame_dispose(GObject *obj) {
  OflFrame *self = OFL_FRAME(obj);
  g_clear_object(&self->file);
  g_clear_object(&self->thumbnail);
  G_OBJECT_CLASS(ofl_frame_parent_class)->dispose(obj);
}

enum {
  SIG_THUMBNAIL_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };

static void ofl_frame_class_init(OflFrameClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = ofl_frame_dispose;

  signals[SIG_THUMBNAIL_CHANGED] = g_signal_new(
    "thumbnail-changed",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE,
    1,
    GDK_TYPE_TEXTURE
  );
}

static void ofl_frame_init(OflFrame *self) {
  self->rotate_steps = 0;
  self->thumbnail = NULL;
  self->full_w = 0;
  self->full_h = 0;
  ofl_adjustments_init_defaults(&self->adjustments);
  ofl_crop_init_defaults(&self->crop);
}

OflFrame* ofl_frame_new(GFile *file) {
  OflFrame *self = g_object_new(OFL_TYPE_FRAME, NULL);
  self->file = g_object_ref(file);
  self->rotate_steps = 0;
  self->thumbnail = NULL;
  self->full_w = 0;
  self->full_h = 0;
  ofl_adjustments_init_defaults(&self->adjustments);
  ofl_crop_init_defaults(&self->crop);
  return self;
}

GFile* ofl_frame_get_file(OflFrame *self) {
  return self->file;
}

int ofl_frame_get_rotate_steps(OflFrame *self) {
  return self->rotate_steps;
}

void ofl_frame_set_rotate_steps(OflFrame *self, int steps) {
  self->rotate_steps = steps % 4;
  if (self->rotate_steps < 0) self->rotate_steps += 4;
}

const OflAdjustments* ofl_frame_get_adjustments(OflFrame *self) {
  return &self->adjustments;
}

void ofl_frame_set_adjustments(OflFrame *self, const OflAdjustments *adj) {
  if (!adj) {
    ofl_adjustments_init_defaults(&self->adjustments);
    return;
  }
  self->adjustments = *adj;
}

const OflCrop* ofl_frame_get_crop(OflFrame *self) {
  return &self->crop;
}

void ofl_frame_set_crop(OflFrame *self, const OflCrop *crop) {
  if (!crop) {
    ofl_crop_init_defaults(&self->crop);
    return;
  }
  self->crop = *crop;
}

void ofl_frame_get_full_size(OflFrame *self, int *out_w, int *out_h) {
  if (out_w) *out_w = self ? self->full_w : 0;
  if (out_h) *out_h = self ? self->full_h : 0;
}

void ofl_frame_set_full_size(OflFrame *self, int w, int h) {
  if (self) {
    self->full_w = w;
    self->full_h = h;
  }
}

GdkTexture* ofl_frame_get_thumbnail(OflFrame *self) {
  return self->thumbnail;
}

void ofl_frame_set_thumbnail(OflFrame *self, GdkTexture *tex) {
  if (self->thumbnail == tex) return;
  if (self->thumbnail) g_object_unref(self->thumbnail);
  self->thumbnail = tex ? g_object_ref(tex) : NULL;
  g_signal_emit(self, signals[SIG_THUMBNAIL_CHANGED], 0, self->thumbnail);
}