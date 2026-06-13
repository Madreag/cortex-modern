// Stand-in for <endian.h> on toolchains without one; little-endian targets only
#ifndef DETMATH_ENDIAN_SHIM_H
#define DETMATH_ENDIAN_SHIM_H

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif

#endif
