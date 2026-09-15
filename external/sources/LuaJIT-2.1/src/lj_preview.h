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

typedef struct LJPreview {
  global_State *g;
  LJPreviewTable *tables;
  GCobj **objects;
  GCobj **seen;
  size_t ntables, tablescap, nobjects, objectscap, nseen, seencap;
  GCtab *root;
  size_t savedbytes;
  uint32_t lastwrite;
  int active;
  int timed;
  double window_ms;
  luaJIT_PreviewStats stats;
} LJPreview;

LJ_FUNCA void lj_preview_write(lua_State *L, GCtab *t);
LJ_FUNC GCtab *lj_preview_saved(global_State *g, GCtab *t);
LJ_FUNC int lj_preview_weak(global_State *g, GCtab *t);
LJ_FUNC void lj_preview_forget(global_State *g, GCtab *t);
LJ_FUNC void lj_preview_free(global_State *g);

#endif
