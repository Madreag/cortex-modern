/*
** Math helper functions for assembler VM.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_vmmath_c
#define LUA_CORE

#include <errno.h>
#include <math.h>

#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_vm.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "luajit.h"

static luaJIT_MathHooks engine_math;
static int engine_math_installed;

double lj_vm_math_ldexp(double x, int32_t exponent) { return ldexp(x, exponent); }

int luaJIT_set_math_hooks(const luaJIT_MathHooks *hooks)
{
  if (!hooks || !hooks->sin || !hooks->cos || !hooks->tan || !hooks->asin || !hooks->acos || !hooks->atan || !hooks->sinh || !hooks->cosh || !hooks->tanh || !hooks->exp || !hooks->log || !hooks->log2 || !hooks->log10 || !hooks->sqrt || !hooks->pow || !hooks->atan2 || !hooks->fmod) return 0;
  engine_math = *hooks;
  engine_math_installed = 1;
  return 1;
}

int luaJIT_math_policy(lua_State *L)
{
#if LJ_HASJIT
  if (L && (G2J(G(L))->flags & JIT_F_OPT_FMA)) return 0;
#endif
  return engine_math_installed;
}

unsigned int luaJIT_runtime_flags(lua_State *L)
{
#if LJ_HASJIT
  return G2J(G(L))->flags;
#else
  UNUSED(L);
  return 0;
#endif
}

double lj_vm_math_sin(double x) { return engine_math.sin ? engine_math.sin(x) : sin(x); }
double lj_vm_math_cos(double x) { return engine_math.cos ? engine_math.cos(x) : cos(x); }
double lj_vm_math_tan(double x) { return engine_math.tan ? engine_math.tan(x) : tan(x); }
double lj_vm_math_asin(double x) { return engine_math.asin ? engine_math.asin(x) : asin(x); }
double lj_vm_math_acos(double x) { return engine_math.acos ? engine_math.acos(x) : acos(x); }
double lj_vm_math_atan(double x) { return engine_math.atan ? engine_math.atan(x) : atan(x); }
double lj_vm_math_sinh(double x) { return engine_math.sinh ? engine_math.sinh(x) : sinh(x); }
double lj_vm_math_cosh(double x) { return engine_math.cosh ? engine_math.cosh(x) : cosh(x); }
double lj_vm_math_tanh(double x) { return engine_math.tanh ? engine_math.tanh(x) : tanh(x); }
double lj_vm_math_exp(double x) { return engine_math.exp ? engine_math.exp(x) : exp(x); }
double lj_vm_math_log(double x) { return engine_math.log ? engine_math.log(x) : log(x); }
double lj_vm_math_log2(double x) {
  if (engine_math.log2) return engine_math.log2(x);
#ifdef LUAJIT_NO_LOG2
  return log(x) * 1.4426950408889634074;
#else
  return log2(x);
#endif
}
double lj_vm_math_log10(double x) { return engine_math.log10 ? engine_math.log10(x) : log10(x); }
double lj_vm_math_sqrt(double x) { return engine_math.sqrt ? engine_math.sqrt(x) : sqrt(x); }
double lj_vm_math_pow(double x, double y) { return engine_math.pow ? engine_math.pow(x, y) : pow(x, y); }
double lj_vm_math_atan2(double x, double y) { return engine_math.atan2 ? engine_math.atan2(x, y) : atan2(x, y); }
double lj_vm_math_fmod(double x, double y) { return engine_math.fmod ? engine_math.fmod(x, y) : fmod(x, y); }

/* -- Wrapper functions --------------------------------------------------- */

#if LJ_TARGET_X86 && __ELF__ && __PIC__
/* Wrapper functions to deal with the ELF/x86 PIC disaster. */
LJ_FUNCA double lj_wrap_log(double x) { return lj_vm_math_log(x); }
LJ_FUNCA double lj_wrap_log10(double x) { return lj_vm_math_log10(x); }
LJ_FUNCA double lj_wrap_exp(double x) { return lj_vm_math_exp(x); }
LJ_FUNCA double lj_wrap_sin(double x) { return lj_vm_math_sin(x); }
LJ_FUNCA double lj_wrap_cos(double x) { return lj_vm_math_cos(x); }
LJ_FUNCA double lj_wrap_tan(double x) { return lj_vm_math_tan(x); }
LJ_FUNCA double lj_wrap_asin(double x) { return lj_vm_math_asin(x); }
LJ_FUNCA double lj_wrap_acos(double x) { return lj_vm_math_acos(x); }
LJ_FUNCA double lj_wrap_atan(double x) { return lj_vm_math_atan(x); }
LJ_FUNCA double lj_wrap_sinh(double x) { return lj_vm_math_sinh(x); }
LJ_FUNCA double lj_wrap_cosh(double x) { return lj_vm_math_cosh(x); }
LJ_FUNCA double lj_wrap_tanh(double x) { return lj_vm_math_tanh(x); }
LJ_FUNCA double lj_wrap_atan2(double x, double y) { return lj_vm_math_atan2(x, y); }
LJ_FUNCA double lj_wrap_pow(double x, double y) { return lj_vm_math_pow(x, y); }
LJ_FUNCA double lj_wrap_fmod(double x, double y) { return lj_vm_math_fmod(x, y); }
#endif

/* -- Helper functions ---------------------------------------------------- */

/* Required to prevent the C compiler from applying FMA optimizations.
**
** Yes, there's -ffp-contract and the FP_CONTRACT pragma ... in theory.
** But the current state of C compilers is a mess in this regard.
** Also, this function is not performance sensitive at all.
*/
LJ_NOINLINE static double lj_vm_floormul(double x, double y)
{
  return lj_vm_floor(x / y) * y;
}

double lj_vm_foldarith(double x, double y, int op)
{
  switch (op) {
  case IR_ADD - IR_ADD: return x+y; break;
  case IR_SUB - IR_ADD: return x-y; break;
  case IR_MUL - IR_ADD: return x*y; break;
  case IR_DIV - IR_ADD: return x/y; break;
  case IR_MOD - IR_ADD: return x-lj_vm_floormul(x, y); break;
  case IR_POW - IR_ADD: return lj_vm_math_pow(x, y); break;
  case IR_NEG - IR_ADD: return -x; break;
  case IR_ABS - IR_ADD: return fabs(x); break;
#if LJ_HASJIT
  case IR_LDEXP - IR_ADD: return ldexp(x, lj_num2int(y)); break;
  case IR_MIN - IR_ADD: return x < y ? x : y; break;
  case IR_MAX - IR_ADD: return x > y ? x : y; break;
#endif
  default: return x;
  }
}

/* -- Helper functions for generated machine code ------------------------- */

#if (LJ_HASJIT && !(LJ_TARGET_ARM || LJ_TARGET_ARM64 || LJ_TARGET_PPC)) || LJ_TARGET_MIPS
int32_t LJ_FASTCALL lj_vm_modi(int32_t a, int32_t b)
{
  uint32_t y, ua, ub;
  /* This must be checked before using this function. */
  lj_assertX(b != 0, "modulo with zero divisor");
  ua = a < 0 ? ~(uint32_t)a+1u : (uint32_t)a;
  ub = b < 0 ? ~(uint32_t)b+1u : (uint32_t)b;
  y = ua % ub;
  if (y != 0 && (a^b) < 0) y = y - ub;
  if (((int32_t)y^b) < 0) y = ~y+1u;
  return (int32_t)y;
}
#endif

#if LJ_HASJIT

#ifdef LUAJIT_NO_LOG2
double lj_vm_log2(double a)
{
  return lj_vm_math_log2(a);
}
#endif

/* Computes fpm(x) for extended math functions. */
double lj_vm_foldfpm(double x, int fpm)
{
  switch (fpm) {
  case IRFPM_FLOOR: return lj_vm_floor(x);
  case IRFPM_CEIL: return lj_vm_ceil(x);
  case IRFPM_TRUNC: return lj_vm_trunc(x);
  case IRFPM_SQRT: return lj_vm_math_sqrt(x);
  case IRFPM_LOG: return lj_vm_math_log(x);
  case IRFPM_LOG2: return lj_vm_log2(x);
  default: lj_assertX(0, "bad fpm %d", fpm);
  }
  return 0;
}

#if LJ_HASFFI
int lj_vm_errno(void)
{
  return errno;
}
#endif

#endif
