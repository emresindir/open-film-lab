#pragma once
#include <gio/gio.h>
#include <glib-object.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

#define OFL_TYPE_FRAME (ofl_frame_get_type())
G_DECLARE_FINAL_TYPE(OflFrame, ofl_frame, OFL, FRAME, GObject)

typedef struct {
  float temperature; // -100.0 to +100.0 (default 0.0)
  float tint;        // -100.0 to +100.0 (default 0.0)
  float exposure;    // -2.0 to +2.0 stops EV (default 0.0)
  float contrast;    // -100.0 to +100.0 (default 0.0)
  float brightness;  // -100.0 to +100.0 (default 0.0)
} OflAdjustments;

typedef struct {
  gboolean enabled; // TRUE if crop is active
  float x;          // normalized [0.0 .. 1.0]
  float y;          // normalized [0.0 .. 1.0]
  float width;      // normalized (0.0 .. 1.0]
  float height;     // normalized (0.0 .. 1.0]
} OflCrop;

void ofl_adjustments_init_defaults(OflAdjustments *adj);
void ofl_crop_init_defaults(OflCrop *crop);

OflFrame* ofl_frame_new(GFile *file);

GFile* ofl_frame_get_file(OflFrame *self);

int  ofl_frame_get_rotate_steps(OflFrame *self);
void ofl_frame_set_rotate_steps(OflFrame *self, int steps);

const OflAdjustments* ofl_frame_get_adjustments(OflFrame *self);
void                  ofl_frame_set_adjustments(OflFrame *self, const OflAdjustments *adj);

const OflCrop* ofl_frame_get_crop(OflFrame *self);
void           ofl_frame_set_crop(OflFrame *self, const OflCrop *crop);

void ofl_frame_get_full_size(OflFrame *self, int *out_w, int *out_h);
void ofl_frame_set_full_size(OflFrame *self, int w, int h);

GdkTexture* ofl_frame_get_thumbnail(OflFrame *self);
void        ofl_frame_set_thumbnail(OflFrame *self, GdkTexture *tex);

G_END_DECLS