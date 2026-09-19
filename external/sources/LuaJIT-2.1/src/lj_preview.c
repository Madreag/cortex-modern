#define lj_preview_c
#define LUA_CORE

#include <stdlib.h>
#include <math.h>
#ifndef NAN
static double preview_nan(void)
{
  volatile double z = 0.0;
  return z / z;
}
#define NAN preview_nan()
#endif
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_dispatch.h"
#include "lj_trace.h"
#include "lj_preview.h"
#include "luajit.h"

#if LJ_TARGET_WINDOWS
#include <windows.h>
#else
#include <time.h>
#endif

static double preview_clock(void)
{
#if LJ_TARGET_WINDOWS
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#elif LJ_TARGET_POSIX
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
#else
  return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#endif
}

static void *preview_grow(LJPreview *p, void *buffer, size_t *capacity, size_t count, size_t size)
{
  global_State *g = p->g;
  size_t next = *capacity ? *capacity : 64;
  void *block;
  if (count <= *capacity) return buffer;
  while (next < count) {
    if (next > SIZE_MAX/2) return NULL;
    next *= 2;
  }
  if (next > SIZE_MAX/size) return NULL;
  block = g->allocf(g->allocd, buffer, *capacity*size, next*size);
  if (!block) return NULL;
  *capacity = next;
  return block;
}

static size_t preview_hash(GCobj *o, size_t mask)
{
  uintptr_t p = (uintptr_t)o;
  return ((p >> 3) * (uintptr_t)2654435761u) & mask;
}

static int preview_seen(LJPreview *p, GCobj *o)
{
  global_State *g = p->g;
  size_t i;
  if (2*(p->nseen+1) >= p->seencap) {
    size_t cap = p->seencap ? p->seencap*2 : 128;
    if (p->seencap > SIZE_MAX/(2*sizeof(GCobj *))) return -1;
    GCobj **seen = (GCobj **)g->allocf(g->allocd, NULL, 0, cap*sizeof(GCobj *));
    if (!seen) return -1;
    memset(seen, 0, cap*sizeof(GCobj *));
    for (i = 0; i < p->seencap; i++) {
      GCobj *old = p->seen[i];
      if (old) {
        size_t at = preview_hash(old, cap-1);
        while (seen[at]) at = (at+1) & (cap-1);
        seen[at] = old;
      }
    }
    if (p->seen) g->allocf(g->allocd, p->seen, p->seencap*sizeof(GCobj *), 0);
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
  objects = (GCobj **)preview_grow(p, p->objects, &p->objectscap,
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

/* Records an upvalue slot as the window found it, or counts it when the window left it changed. */
static int preview_upvalue(LJPreview *p, GCupval *uv, cTValue *v, int scan)
{
  LJPreviewUV *entry, *upvalues;
  if (scan) {
    size_t lo = 0, hi = p->nupvalues;
    while (lo < hi) {
      size_t mid = lo + (hi-lo)/2;
      if (p->upvalues[mid].uv < uv) lo = mid+1; else hi = mid;
    }
    if (lo < p->nupvalues && p->upvalues[lo].uv == uv) {
      entry = &p->upvalues[lo];
      if (!entry->counted && entry->saved.u64 != v->u64) {
	entry->counted = 1;
	p->stats.upvalue_writes++;
      }
    }
    return 1;
  }
  upvalues = (LJPreviewUV *)preview_grow(p, p->upvalues, &p->upvaluescap,
				        p->nupvalues+1, sizeof(LJPreviewUV));
  if (!upvalues) return 0;
  p->upvalues = upvalues;
  entry = &p->upvalues[p->nupvalues++];
  entry->uv = uv;
  entry->saved = *v;
  entry->counted = 0;
  return 1;
}

static int preview_by_uv(const void *a, const void *b)
{
  const GCupval *x = ((const LJPreviewUV *)a)->uv, *y = ((const LJPreviewUV *)b)->uv;
  return (x > y) - (x < y);
}

/* Walks what the window covers, from the roots already seeded. Scanning only revisits upvalue slots. */
static int preview_walk(LJPreview *p, int scan)
{
  size_t cursor, i;
  for (cursor = 0; cursor < p->nobjects; cursor++) {
    GCobj *o = p->objects[cursor];
    if (o->gch.gct == ~LJ_TTAB) {
      GCtab *t = gco2tab(o);
      if (!scan) {
	LJPreviewTable *entry, *tables;
	if (p->ntables == LJ_PREVIEW_INDEX) return 0;
	tables = (LJPreviewTable *)preview_grow(p, p->tables, &p->tablescap,
					       p->ntables+1, sizeof(LJPreviewTable));
	if (!tables) return 0;
	p->tables = tables;
	entry = &p->tables[p->ntables++];
	memset(entry, 0, sizeof(*entry));
	entry->table = t;
	t->preview = LJ_PREVIEW_PENDING | (uint32_t)p->ntables;
      }
      if (!preview_object(p, gcref(t->metatable))) return 0;
      for (i = 0; i < t->asize; i++)
	if (!preview_value(p, arrayslot(t, i))) return 0;
      if (t->hmask) {
	for (i = 0; i <= t->hmask; i++) {
	  Node *n = &noderef(t->node)[i];
	  if (!tvisnil(&n->val) &&
	      (!preview_value(p, &n->key) || !preview_value(p, &n->val))) return 0;
	}
      }
    } else if (o->gch.gct == ~LJ_TFUNC) {
      GCfunc *fn = gco2func(o);
      if (!preview_object(p, gcref(fn->c.env))) return 0;
      for (i = 0; i < fn->c.nupvalues; i++) {
	cTValue *v;
	if (isluafunc(fn)) {
	  GCupval *uv = &gcref(fn->l.uvptr[i])->uv;
	  v = uvval(uv);
	  /* The barrier puts tables back; a slot is measured, so the contract is decided on numbers. */
	  if (p->measure && !preview_upvalue(p, uv, v, scan)) return 0;
	} else {
	  v = &fn->c.upvalue[i];
	}
	if (!preview_value(p, v)) return 0;
      }
    } else if (o->gch.gct == ~LJ_TUDATA) {
      GCudata *ud = gco2ud(o);
      if (!preview_object(p, gcref(ud->env)) ||
	  !preview_object(p, gcref(ud->metatable))) return 0;
    } else {
      lua_State *th = gco2th(o);
      TValue *v;
      if (!preview_object(p, gcref(th->env))) return 0;
      for (v = tvref(th->stack)+1+LJ_FR2; v < th->top; v++)
	if (!preview_value(p, v)) return 0;
    }
  }
  return 1;
}

static void preview_sample(LJPreview *p, double ms)
{
  global_State *g;
  if (p->nsamples == p->samplescap) {
    size_t cap;
    double *samples;
    if (p->samplescap >= 65536) return;  /* p99 covers the first 64K windows of a timed state. */
    cap = p->samplescap ? p->samplescap*2 : 1024;
    if (cap > 65536) cap = 65536;
    g = p->g;
    samples = (double *)g->allocf(g->allocd, p->samples, p->samplescap*sizeof(double), cap*sizeof(double));
    if (!samples) return;
    p->samples = samples;
    p->samplescap = cap;
  }
  p->samples[p->nsamples++] = ms;
}

static int preview_by_ms(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double preview_p99(LJPreview *p)
{
  global_State *g = p->g;
  double *sorted;
  double result;
  size_t at;
  if (!p->nsamples) return NAN;
  sorted = (double *)g->allocf(g->allocd, NULL, 0, p->nsamples*sizeof(double));
  if (!sorted) return NAN;
  memcpy(sorted, p->samples, p->nsamples*sizeof(double));
  qsort(sorted, p->nsamples, sizeof(double), preview_by_ms);
  at = (p->nsamples*99 + 99)/100 - 1;  /* ceil(0.99*n)-1: the 99th percentile rank. */
  result = sorted[at];
  g->allocf(g->allocd, sorted, p->nsamples*sizeof(double), 0);
  return result;
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
  p->registry = NULL;
  p->ntables = p->nobjects = p->nseen = 0;
  p->nskipped = p->nupvalues = 0;
  p->lastwrite = 0;
  if (p->seencap) memset(p->seen, 0, p->seencap*sizeof(GCobj *));
}

LUA_API int luaJIT_preview_begin(lua_State *L, const char *const *skip, size_t nskip,
				 unsigned int flags)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  GCtab *root = tabref(L->env);
  size_t i;
  int timed = p ? p->timed : getenv("CC_PREVIEW_BARRIER_STATS") != NULL;
  double started = timed ? preview_clock() : 0.0;
#if LJ_HASJIT
  /* The recorded preview guard is hoisted out of loops, so a window must not open inside a trace. */
  if (G2J(g)->state != LJ_TRACE_IDLE)
    lj_trace_abort_leftover(L);
  if (G2J(g)->state != LJ_TRACE_IDLE) return 0;
#endif
  if (!p) {
    p = (LJPreview *)g->allocf(g->allocd, NULL, 0, sizeof(LJPreview));
    if (!p) return 0;
    memset(p, 0, sizeof(LJPreview));
    p->g = g;
    g->preview = p;
    p->timed = timed;
    p->measure = getenv("CC_PREVIEW_UPVALUE_MEASURE") != NULL;
  }
  if (p->active) return 0;
  /* Tables born in a speculative window die with it, so their numbers are handed back. */
  p->savedserial = g->objserial;
  p->savedbytes = 0;
  p->nskipped = 0;
  p->nupvalues = 0;
  if (root->hmask) {
    for (i = 0; i <= root->hmask; i++) {
      Node *node = &noderef(root->node)[i];
      size_t k;
      if (!tvisstr(&node->key) || !tvisgcv(&node->val)) continue;
      for (k = 0; k < nskip; k++) {
	GCobj **skipped;
	if (strV(&node->key)->len != strlen(skip[k]) || strcmp(strVdata(&node->key), skip[k]))
	  continue;
	if (preview_seen(p, gcV(&node->val)) < 0) goto fail;
	/* The measurement walk runs after the window, so it needs the same skips. */
	skipped = (GCobj **)preview_grow(p, p->skipped, &p->skippedcap,
					p->nskipped+1, sizeof(GCobj *));
	if (!skipped) goto fail;
	p->skipped = skipped;
	p->skipped[p->nskipped++] = gcV(&node->val);
      }
    }
  }
  p->registry = (flags & LUAJIT_PREVIEW_REGISTRY_ROOT) ? tabV(registry(L)) : NULL;
  if (!preview_object(p, obj2gco(root))) goto fail;
  if (p->registry && !preview_object(p, obj2gco(p->registry))) goto fail;
  if (!preview_walk(p, 0)) goto fail;
  p->root = root;
  p->active = 1;
  if (timed) {
    p->window_ms = preview_clock()-started;
    p->stats.capture_ms += p->window_ms;
    p->stats.tables += p->ntables;
  }
  if (p->measure) p->stats.upvalues += p->nupvalues;
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
  double started;
  if (!(t->preview & LJ_PREVIEW_PENDING) || !p || !p->active) return;
  if (!index || index > p->ntables) return;  /* A stale word addresses no entry. */
  started = p->timed ? preview_clock() : 0.0;
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
  g->gc.total += abytes+hbytes;  /* Images are live for the window, so pacing must see them. */
  t->preview = index;
  lj_gc_anybarriert(L, t);
  if (p->timed) {
    double elapsed = preview_clock()-started;
    p->window_ms += elapsed;
    p->stats.write_ms += elapsed;
    p->stats.saves++;
    p->stats.bytes += abytes+hbytes;
  }
}

GCtab *lj_preview_saved(global_State *g, GCtab *t)
{
  LJPreview *p = g->preview;
  uint32_t index = t->preview & LJ_PREVIEW_INDEX;
  if (p && p->active && index && index <= p->ntables) {
    LJPreviewTable *e = &p->tables[index-1];
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
      if (tvisstr(&n->key) && strV(&n->key)->len == 6 && tvisstr(&n->val) &&
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
  LJPreview *p = g->preview;
  uint32_t index = t->preview & LJ_PREVIEW_INDEX;
  LJPreviewTable *e;
  if (!p || !index || index > p->ntables) return;  /* A stale word addresses no entry. */
  e = &p->tables[index-1];
  if (e->captured) {
    size_t abytes = e->saved.asize*sizeof(TValue);
    size_t hbytes = e->saved.hmask ? (e->saved.hmask+1)*sizeof(Node) : 0;
    if (abytes) g->allocf(g->allocd, tvref(e->saved.array), abytes, 0);
    if (hbytes) g->allocf(g->allocd, noderef(e->saved.node), hbytes, 0);
    g->gc.total -= abytes+hbytes;
    e->captured = 0;
  }
  e->table = NULL;
}

LUA_API size_t luaJIT_preview_end(lua_State *L)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  size_t changes = 0;
  double started;
  if (!p || !p->active) return 0;
  p->active = 0;
  if (p->measure && p->nupvalues) {
    /* The graph as the window leaves it: a slot that differs is a write no rollback undoes. */
    size_t at, unique = 0;
    double scanned = p->timed ? preview_clock() : 0.0;  /* The scan's own cost, kept out of the window's. */
    qsort(p->upvalues, p->nupvalues, sizeof(LJPreviewUV), preview_by_uv);
    for (at = 0; at < p->nupvalues; at++)
      if (!unique || p->upvalues[unique-1].uv != p->upvalues[at].uv)
	p->upvalues[unique++] = p->upvalues[at];
    p->nupvalues = unique;
    p->nobjects = p->nseen = 0;
    if (p->seencap) memset(p->seen, 0, p->seencap*sizeof(GCobj *));
    for (at = 0; at < p->nskipped; at++)
      if (preview_seen(p, p->skipped[at]) < 0) break;
    if (preview_object(p, obj2gco(p->root)) &&
	(!p->registry || preview_object(p, obj2gco(p->registry))))
      preview_walk(p, 1);
    if (p->timed) p->stats.measure_ms += preview_clock()-scanned;
  }
  started = p->timed ? preview_clock() : 0.0;
  while (p->lastwrite) {
    LJPreviewTable *e = &p->tables[p->lastwrite-1];
    GCtab *t = e->table;
    GCtab *s = &e->saved;
    size_t abytes = s->asize*sizeof(TValue);
    p->lastwrite = e->previous;
    if (!t) continue;
    changes++;
    if (t->hmask) lj_mem_freevec(g, noderef(t->node), t->hmask+1, Node);
    if (t->asize && t->colo <= 0)
      lj_mem_freevec(g, tvref(t->array), t->asize, TValue);
    if (s->colo > 0) {
      TValue *inlinearray = (TValue *)((char *)t+sizeof(GCtab));
      if (abytes) {
        memcpy(inlinearray, tvref(s->array), abytes);
        g->allocf(g->allocd, tvref(s->array), abytes, 0);
        g->gc.total -= abytes;
      }
      setmref(t->array, inlinearray);
    } else {
      setmrefr(t->array, s->array);  /* The image becomes the live vector it was counted as. */
    }
    setmrefr(t->node, s->node);
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
  }
  g->objserial = p->savedserial;
  preview_disarm(p);
  if (p->timed) {
    double elapsed = preview_clock()-started;
    p->window_ms += elapsed;
    p->stats.restore_ms += elapsed;
    if (p->window_ms > p->stats.max_ms) p->stats.max_ms = p->window_ms;
    p->stats.windows++;
    preview_sample(p, p->window_ms);
  }
  return changes;
}

LUA_API int luaJIT_preview_measure(lua_State *L, int on)
{
  LJPreview *p = G(L)->preview;
  int previous;
  if (!p) {
    global_State *g = G(L);
    if (on < 0) return 0;
    p = (LJPreview *)g->allocf(g->allocd, NULL, 0, sizeof(LJPreview));
    if (!p) return 0;
    memset(p, 0, sizeof(LJPreview));
    p->g = g;
    g->preview = p;
    p->timed = getenv("CC_PREVIEW_BARRIER_STATS") != NULL;
    p->measure = getenv("CC_PREVIEW_UPVALUE_MEASURE") != NULL;
  }
  previous = p->measure;
  if (on >= 0 && !p->active) p->measure = on;  /* An armed window keeps the setting it was armed with. */
  return previous;
}

LUA_API size_t luaJIT_preview_upvalue_writes(lua_State *L)
{
  LJPreview *p = G(L)->preview;
  return p ? p->stats.upvalue_writes : 0;
}

LUA_API int luaJIT_preview_registry_rooted(lua_State *L)
{
  LJPreview *p = G(L)->preview;
  return p && p->active && p->registry ? 1 : 0;
}

LUA_API int luaJIT_preview_stats(lua_State *L, luaJIT_PreviewStats *stats)
{
  LJPreview *p = G(L)->preview;
  if (!p || !p->timed || !p->stats.windows) return 0;
  *stats = p->stats;
  stats->p99_ms = preview_p99(p);
  return 1;
}

/* Fault injection: a freed window and a corrupted index must both leave the barrier untouched. */
LUA_API int luaJIT_preview_faultcheck(lua_State *L)
{
  global_State *g = G(L);
  LJPreview *p = g->preview;
  uint32_t stale = LJ_PREVIEW_PENDING | (uint32_t)(p ? p->ntables+1 : 1);
  uint32_t lastwrite = p ? p->lastwrite : 0;
  size_t savedbytes = p ? p->savedbytes : 0;
  GCtab *t;
  int ok;
  lua_createtable(L, 0, 0);
  t = tabV(L->top-1);
  g->preview = NULL;  /* The state lj_preview_free leaves behind. */
  t->preview = LJ_PREVIEW_PENDING | 1;
  lj_preview_write(L, t);
  lj_preview_forget(g, t);
  g->preview = p;
  t->preview = stale;
  lj_preview_write(L, t);
  lj_preview_forget(g, t);
  ok = t->preview == stale &&
       (!p || (p->lastwrite == lastwrite && p->savedbytes == savedbytes));
  t->preview = 0;
  lua_pop(L, 1);
  return ok;
}

void lj_preview_free(global_State *g)
{
  LJPreview *p = g->preview;
  if (!p) return;
  luaJIT_preview_end(mainthread(g));
  if (p->tables) g->allocf(g->allocd, p->tables, p->tablescap*sizeof(LJPreviewTable), 0);
  if (p->objects) g->allocf(g->allocd, p->objects, p->objectscap*sizeof(GCobj *), 0);
  if (p->seen) g->allocf(g->allocd, p->seen, p->seencap*sizeof(GCobj *), 0);
  if (p->skipped) g->allocf(g->allocd, p->skipped, p->skippedcap*sizeof(GCobj *), 0);
  if (p->upvalues) g->allocf(g->allocd, p->upvalues, p->upvaluescap*sizeof(LJPreviewUV), 0);
  if (p->samples) g->allocf(g->allocd, p->samples, p->samplescap*sizeof(double), 0);
  g->allocf(g->allocd, p, sizeof(LJPreview), 0);
  g->preview = NULL;
}
