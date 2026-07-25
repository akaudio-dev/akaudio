/* Minimal build config for the FAAD2 vendored into akaudio (src/dep/faad2).
 *
 * FAAD2's libfaad/common.h reaches for this via `#ifdef HAVE_CONFIG_H` — the
 * Makefile defines HAVE_CONFIG_H *only* for the FAAD2 objects (not globally, so
 * the vendored libogg/libvorbis, which also test HAVE_CONFIG_H, are unaffected).
 *
 * Its sole job: select the modern hosted-libc code paths in common.h. On MinGW
 * and glibc, common.h takes its POSIX branch (`__MINGW32__` is defined even on
 * 64-bit mingw-w64, so the plain-_WIN32 branch is skipped); without these macros
 * that branch falls back to pre-C89 `bcopy()`/`index()` stubs and hand-rolled
 * uint typedefs that don't compile against a current toolchain. Defining them
 * makes it `#include <string.h>`/`<stdint.h>` and use the real functions.
 *
 * macOS never compiles FAAD2 (it uses AudioToolbox), so this is Windows/Linux only.
 * FAAD2 defaults otherwise stand: a float build with SBR + PS (full HE-AAC v2).
 */
#ifndef AKAUDIO_FAAD2_CONFIG_H
#define AKAUDIO_FAAD2_CONFIG_H

#define STDC_HEADERS    1  /* <stdlib.h>, <stddef.h> */
#define HAVE_STRING_H   1  /* <string.h> — real memcpy/memmove/strchr */
#define HAVE_STDINT_H   1  /* <stdint.h> — real int{8,16,32,64}_t */
#define HAVE_INTTYPES_H 1  /* <inttypes.h> (pulls in <stdint.h>) */
#define HAVE_STDLIB_H   1
#define HAVE_MEMCPY     1
#define HAVE_STRCHR     1
#define HAVE_LRINTF     1  /* mingw + glibc both provide lrintf() */

/* decoder.c hard-#errors without this; NeAACDecGetVersion() reports it. */
#define PACKAGE_VERSION "2.11.1"

#endif
