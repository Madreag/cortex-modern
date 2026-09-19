/*
** LuaJIT -- a Just-In-Time Compiler for Lua. https://luajit.org/
**
** Copyright (C) 2005-2026 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: https://www.opensource.org/licenses/mit-license.php ]
*/

#ifndef _LUAJIT_H
#define _LUAJIT_H

#include "lua.h"

#define LUAJIT_VERSION		"LuaJIT 2.1.ROLLING"
#define LUAJIT_VERSION_NUM	20199  /* Deprecated. */
#define LUAJIT_VERSION_SYM	luaJIT_version_2_1_ROLLING
#define LUAJIT_COPYRIGHT	"Copyright (C) 2005-2026 Mike Pall"
#define LUAJIT_URL		"https://luajit.org/"

/* Modes for luaJIT_setmode. */
#define LUAJIT_MODE_MASK	0x00ff

enum {
  LUAJIT_MODE_ENGINE,		/* Set mode for whole JIT engine. */
  LUAJIT_MODE_DEBUG,		/* Set debug mode (idx = level). */

  LUAJIT_MODE_FUNC,		/* Change mode for a function. */
  LUAJIT_MODE_ALLFUNC,		/* Recurse into subroutine protos. */
  LUAJIT_MODE_ALLSUBFUNC,	/* Change only the subroutines. */

  LUAJIT_MODE_TRACE,		/* Flush a compiled trace. */

  LUAJIT_MODE_WRAPCFUNC = 0x10,	/* Set wrapper mode for C function calls. */

  LUAJIT_MODE_MAX
};

/* Flags or'ed in to the mode. */
#define LUAJIT_MODE_OFF		0x0000	/* Turn feature off. */
#define LUAJIT_MODE_ON		0x0100	/* Turn feature on. */
#define LUAJIT_MODE_FLUSH	0x0200	/* Flush JIT-compiled code. */

/* LuaJIT public C API. */

/* Control the JIT engine. */
LUA_API int luaJIT_setmode(lua_State *L, int idx, int mode);

/* Native preview boundary; no Lua library entry points. */
#define LUAJIT_PREVIEW_REGISTRY_ROOT	0x0001	/* Roll the registry back with the globals. */
LUA_API int luaJIT_preview_begin(lua_State *L, const char *const *skip, size_t nskip,
				 unsigned int flags);
LUA_API size_t luaJIT_preview_end(lua_State *L);
typedef struct luaJIT_PreviewStats {
  size_t windows, tables, saves, bytes, upvalues, upvalue_writes;
  double capture_ms, write_ms, restore_ms, max_ms, p99_ms, measure_ms;
} luaJIT_PreviewStats;
LUA_API int luaJIT_preview_stats(lua_State *L, luaJIT_PreviewStats *stats);
/* Upvalue-slot measurement: on = 1 enables, 0 disables, -1 only reads. Returns the setting before the call. */
LUA_API int luaJIT_preview_measure(lua_State *L, int on);
/* Slots are matched by address, so an upvalue freed and its address reused inside a window is mis-attributed. */
LUA_API size_t luaJIT_preview_upvalue_writes(lua_State *L);
/* Whether the armed window holds the registry as a rollback root. */
LUA_API int luaJIT_preview_registry_rooted(lua_State *L);

/* Low-overhead profiling API. */
typedef void (*luaJIT_profile_callback)(void *data, lua_State *L,
					int samples, int vmstate);
LUA_API void luaJIT_profile_start(lua_State *L, const char *mode,
				  luaJIT_profile_callback cb, void *data);
LUA_API void luaJIT_profile_stop(lua_State *L);
LUA_API const char *luaJIT_profile_dumpstack(lua_State *L, const char *fmt,
					     int depth, size_t *len);

/* Enforce (dynamic) linker error for version mismatches. Call from main. */
LUA_API void LUAJIT_VERSION_SYM(void);

#error "DO NOT USE luajit_rolling.h -- only include build-generated luajit.h"
#endif
