#ifndef _LJ_PREVIEW_H
#define _LJ_PREVIEW_H

#include "lj_obj.h"
#include "luajit.h"

typedef struct LJPreviewTable {
  GCtab *table;
  GCtab saved;
  uint32_t previous;
  int captured;
} LJPreviewTable;

/* An upvalue slot as the window found it; the measurement compares it at the end. */
typedef struct LJPreviewUV {
  GCupval *uv;
  TValue saved;
  int counted;
} LJPreviewUV;

typedef struct LJPreview {
  global_State *g;
  LJPreviewTable *tables;
  GCobj **objects;
  GCobj **seen;
  GCobj **skipped;
  LJPreviewUV *upvalues;
  size_t ntables, tablescap, nobjects, objectscap, nseen, seencap;
  size_t nskipped, skippedcap, nupvalues, upvaluescap;
  GCtab *root;
  GCtab *registry;
  size_t savedbytes;
  uint32_t lastwrite;
  int active;
  int timed;
  int measure;
  double window_ms;
  double *samples;
  size_t nsamples, samplescap;
  luaJIT_PreviewStats stats;
} LJPreview;

LJ_FUNCA void lj_preview_write(lua_State *L, GCtab *t);
LJ_FUNC GCtab *lj_preview_saved(global_State *g, GCtab *t);
LJ_FUNC int lj_preview_weak(global_State *g, GCtab *t);
LJ_FUNC void lj_preview_forget(global_State *g, GCtab *t);
LJ_FUNC void lj_preview_free(global_State *g);

#endif
