#define lj_preview_c
#define LUA_CORE

#include <stdlib.h>
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_frame.h"
#include "lj_preview.h"
#include "luajit.h"

static void *preview_grow(void *buffer, size_t *capacity, size_t count, size_t size)
{
  size_t next = *capacity ? *capacity : 64;
  void *p;
  if (count <= *capacity) return buffer;
  while (next < count) {
    if (next > SIZE_MAX/2) return NULL;
    next *= 2;
  }
  if (next > SIZE_MAX/size) return NULL;
  p = realloc(buffer, next*size);
  if (!p) return NULL;
  *capacity = next;
  return p;
}

static size_t preview_hash(GCobj *o, size_t mask)
{
  uintptr_t p = (uintptr_t)o;
  return ((p >> 3) * (uintptr_t)2654435761u) & mask;
}

static int preview_seen(LJPreview *p, GCobj *o)
{
  size_t i;
  if (2*(p->nseen+1) >= p->seencap) {
    size_t cap = p->seencap ? p->seencap*2 : 128;
    if (p->seencap > SIZE_MAX/(2*sizeof(GCobj *))) return -1;
    GCobj **seen = (GCobj **)calloc(cap, sizeof(GCobj *));
    if (!seen) return -1;
    for (i = 0; i < p->seencap; i++) {
      GCobj *old = p->seen[i];
      if (old) {
        size_t at = preview_hash(old, cap-1);
        while (seen[at]) at = (at+1) & (cap-1);
        seen[at] = old;
      }
    }
    free(p->seen);
    p->seen = seen;
    p->seencap = cap;
  }
  i = preview_hash(o, p->seencap-1);
  while (p->seen[i]) {
    if (p->seen[i] == o) return 1;
    i = (i+1) & (p->seencap-1);
  }
  p->seen[i] = o;
  p->nseen++;
  return 0;
}

static int preview_object(LJPreview *p, GCobj *o)
{
  int seen;
  GCobj **objects;
  if (!o || !(o->gch.gct == ~LJ_TTAB || o->gch.gct == ~LJ_TFUNC ||
              o->gch.gct == ~LJ_TUDATA || o->gch.gct == ~LJ_TTHREAD)) return 1;
  seen = preview_seen(p, o);
  if (seen) return seen > 0;
  objects = (GCobj **)preview_grow(p->objects, &p->objectscap,
                                  p->nobjects+1, sizeof(GCobj *));
  if (!objects) return 0;
  p->objects = objects;
  p->objects[p->nobjects++] = o;
  return 1;
}

static int preview_value(LJPreview *p, cTValue *v)
{
  return !tvisgcv(v) || preview_object(p, gcV(v));
}

static void preview_disarm(LJPreview *p)
{
  size_t i;
  for (i = 0; i < p->ntables; i++) {
    GCtab *t = p->tables[i].table;
    if (t) t->preview = 0;
  }
  p->active = 0;
  p->root = NULL;
  p->ntables = p->nobjects = p->nseen = 0;
  p->lastwrite = 0;
  if (p->seencap) memset(p->seen, 0, p->seencap*sizeof(GCobj *));
}

LUA_API int luaJIT_preview_begin(lua_State *L, const char *const *skip, size_t nskip)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  GCtab *root = tabref(L->env);
  size_t i, cursor;
  if (!p) {
    p = (LJPreview *)calloc(1, sizeof(LJPreview));
    if (!p) return 0;
    g->preview = p;
  }
  if (p->active) return 0;
  p->savedbytes = 0;
  if (root->hmask) {
    for (i = 0; i <= root->hmask; i++) {
      Node *node = &noderef(root->node)[i];
      size_t k;
      if (!tvisstr(&node->key) || !tvisgcv(&node->val)) continue;
      for (k = 0; k < nskip; k++) {
        if (!strcmp(strVdata(&node->key), skip[k]) &&
            preview_seen(p, gcV(&node->val)) < 0) goto fail;
      }
    }
  }
  if (!preview_object(p, obj2gco(root))) goto fail;
  for (cursor = 0; cursor < p->nobjects; cursor++) {
    GCobj *o = p->objects[cursor];
    if (o->gch.gct == ~LJ_TTAB) {
      GCtab *t = gco2tab(o);
      LJPreviewTable *entry, *tables;
      if (p->ntables == LJ_PREVIEW_INDEX) goto fail;
      tables = (LJPreviewTable *)preview_grow(p->tables, &p->tablescap,
                                             p->ntables+1, sizeof(LJPreviewTable));
      if (!tables) goto fail;
      p->tables = tables;
      entry = &p->tables[p->ntables++];
      memset(entry, 0, sizeof(*entry));
      entry->table = t;
      t->preview = LJ_PREVIEW_PENDING | (uint32_t)p->ntables;
      if (!preview_object(p, gcref(t->metatable))) goto fail;
      for (i = 0; i < t->asize; i++)
        if (!preview_value(p, arrayslot(t, i))) goto fail;
      if (t->hmask) {
        for (i = 0; i <= t->hmask; i++) {
          Node *n = &noderef(t->node)[i];
          if (!tvisnil(&n->val) &&
              (!preview_value(p, &n->key) || !preview_value(p, &n->val))) goto fail;
        }
      }
    } else if (o->gch.gct == ~LJ_TFUNC) {
      GCfunc *fn = gco2func(o);
      if (!preview_object(p, gcref(fn->c.env))) goto fail;
      for (i = 0; i < fn->c.nupvalues; i++) {
        cTValue *v = isluafunc(fn) ? uvval(&gcref(fn->l.uvptr[i])->uv) : &fn->c.upvalue[i];
        if (!preview_value(p, v)) goto fail;
      }
    } else if (o->gch.gct == ~LJ_TUDATA) {
      GCudata *ud = gco2ud(o);
      if (!preview_object(p, gcref(ud->env)) ||
          !preview_object(p, gcref(ud->metatable))) goto fail;
    } else {
      lua_State *th = gco2th(o);
      TValue *v, *frame;
      if (!preview_object(p, gcref(th->env))) goto fail;
      for (v = tvref(th->stack)+1+LJ_FR2; v < th->top; v++)
        if (!preview_value(p, v)) goto fail;
      for (frame = th->base-1; frame > tvref(th->stack)+LJ_FR2; frame = frame_prev(frame))
        if (!preview_object(p, obj2gco(frame_func(frame)))) goto fail;
    }
  }
  p->root = root;
  p->active = 1;
  return 1;
fail:
  preview_disarm(p);
  return 0;
}

void lj_preview_write(lua_State *L, GCtab *t)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  LJPreviewTable *e;
  TValue *array = NULL;
  Node *nodes = NULL;
  size_t i, abytes, hbytes;
  uint32_t index = t->preview & LJ_PREVIEW_INDEX;
  if (!(t->preview & LJ_PREVIEW_PENDING) || !p || !p->active) return;
  e = &p->tables[index-1];
  lj_assertL(e->table == t && !e->captured, "bad preview table");
  abytes = t->asize*sizeof(TValue);
  hbytes = t->hmask ? (t->hmask+1)*sizeof(Node) : 0;
  if (abytes) {
    array = (TValue *)g->allocf(g->allocd, NULL, 0, abytes);
    if (!array) lj_err_mem(L);
    memcpy(array, tvref(t->array), abytes);
  }
  if (hbytes) {
    nodes = (Node *)g->allocf(g->allocd, NULL, 0, hbytes);
    if (!nodes) {
      if (array) g->allocf(g->allocd, array, abytes, 0);
      lj_err_mem(L);
    }
    memcpy(nodes, noderef(t->node), hbytes);
    for (i = 0; i <= t->hmask; i++) {
      Node *next = nextnode(&nodes[i]);
      if (next) setmref(nodes[i].next, nodes+(next-noderef(t->node)));
    }
  }
  e->saved = *t;
  setmref(e->saved.array, array);
  if (nodes) {
    setmref(e->saved.node, nodes);
    setfreetop(&e->saved, nodes, nodes+(getfreetop(t, noderef(t->node))-noderef(t->node)));
  }
  e->captured = 1;
  e->previous = p->lastwrite;
  p->lastwrite = index;
  p->savedbytes += abytes+hbytes;
  t->preview = index;
  lj_gc_anybarriert(L, t);
}

GCtab *lj_preview_saved(global_State *g, GCtab *t)
{
  LJPreview *p = g->preview;
  if (p && p->active && t->preview) {
    LJPreviewTable *e = &p->tables[(t->preview & LJ_PREVIEW_INDEX)-1];
    if (e->captured) return &e->saved;
  }
  return NULL;
}

int lj_preview_weak(global_State *g, GCtab *t)
{
  GCtab *mt = tabref(t->metatable);
  GCtab *saved;
  size_t i;
  if (!mt) return 0;
  saved = lj_preview_saved(g, mt);
  if (saved) mt = saved;
  if (mt->hmask) {
    for (i = 0; i <= mt->hmask; i++) {
      Node *n = &noderef(mt->node)[i];
      if (tvisstr(&n->key) && tvisstr(&n->val) &&
          !strcmp(strVdata(&n->key), "__mode")) {
        const char *mode = strVdata(&n->val);
        return (strchr(mode, 'k') ? LJ_GC_WEAKKEY : 0) |
               (strchr(mode, 'v') ? LJ_GC_WEAKVAL : 0);
      }
    }
  }
  return 0;
}

void lj_preview_forget(global_State *g, GCtab *t)
{
  LJPreviewTable *e = &g->preview->tables[(t->preview & LJ_PREVIEW_INDEX)-1];
  if (e->captured) {
    if (e->saved.asize)
      g->allocf(g->allocd, tvref(e->saved.array), e->saved.asize*sizeof(TValue), 0);
    if (e->saved.hmask)
      g->allocf(g->allocd, noderef(e->saved.node), (e->saved.hmask+1)*sizeof(Node), 0);
    e->captured = 0;
  }
  e->table = NULL;
}

LUA_API size_t luaJIT_preview_end(lua_State *L)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  size_t changes = 0;
  if (!p || !p->active) return 0;
  p->active = 0;
  while (p->lastwrite) {
    LJPreviewTable *e = &p->tables[p->lastwrite-1];
    GCtab *t = e->table;
    GCtab *s = &e->saved;
    size_t abytes = s->asize*sizeof(TValue);
    size_t hbytes = s->hmask ? (s->hmask+1)*sizeof(Node) : 0;
    p->lastwrite = e->previous;
    if (!t) continue;
    if (t->hmask) lj_mem_freevec(g, noderef(t->node), t->hmask+1, Node);
    if (t->asize && t->colo <= 0)
      lj_mem_freevec(g, tvref(t->array), t->asize, TValue);
    if (s->colo > 0) {
      TValue *inlinearray = (TValue *)((char *)t+sizeof(GCtab));
      if (abytes) {
        memcpy(inlinearray, tvref(s->array), abytes);
        g->allocf(g->allocd, tvref(s->array), abytes, 0);
      }
      setmref(t->array, inlinearray);
    } else {
      setmrefr(t->array, s->array);
      g->gc.total += abytes;
    }
    setmrefr(t->node, s->node);
    g->gc.total += hbytes;
#if LJ_GC64
    setmrefr(t->freetop, s->freetop);
#endif
    t->asize = s->asize;
    t->hmask = s->hmask;
    t->colo = s->colo;
    t->nomm = 0;
    setgcrefr(t->metatable, s->metatable);
    lj_gc_anybarriert(L, t);
    e->captured = 0;
    changes++;
  }
  preview_disarm(p);
  return changes;
}

void lj_preview_free(global_State *g)
{
  LJPreview *p = g->preview;
  if (!p) return;
  luaJIT_preview_end(mainthread(g));
  free(p->tables);
  free(p->objects);
  free(p->seen);
  free(p);
  g->preview = NULL;
}
