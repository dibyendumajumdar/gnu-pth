# Plan: drop Autoconf, go CMake-only

Deferred task (reverted on 2026-07-10 without committing). CMake already
replicates everything `configure.ac` does and is the CI-validated build; this
plan removes the legacy GNU Autotools build so there is one source of truth.
Redo from a clean HEAD; ideally on a machine with `cmake` installed so the
result can be built and tested locally before pushing.

## Why it's safe

CMake is self-contained. It only consumes:
- `shtool` — runs `scpp` to generate `pth_p.h` (verified it works standalone:
  `sh ./shtool scpp -o pth_p.h -t pth_p.h.in -Dcpp -Cintern -M '==#==' <HSRCS>`).
- the `@var@` templates: `pth.h.in`, `pth_acmac.h.in`, `pthread.h.in`,
  `pth_p.h.in`, `cmake/pth_acdef.h.cmake.in`.

None of these are Autotools-generated; all are kept.

## Files to remove

configure, configure.ac, aclocal.m4, libtool.m4, ltmain.sh, config.guess,
config.sub, config.param, Makefile.in, pth_acdef.h.in, pth.m4,
pth-config.in, pth-config.pod, pth-config.1,
pthread-config.in, pthread-config.pod, pthread-config.1,
pth.spec (autotools-era; `%files` assumes the old layout — remove rather than
half-fix, distros write their own spec for CMake projects),
striptease.mk, striptease.pl (unused single-file extractor tied to the old build;
confirmed zero references).

(In the Cowork sandbox, `rm` needs the `allow_cowork_file_delete` grant first.)

## CMake additions (modern replacements)

1. **pkg-config** (replaces the removed `pth-config` script). Add `pth.pc.in`:

   ```
   prefix=@CMAKE_INSTALL_PREFIX@
   exec_prefix=${prefix}
   libdir=@CMAKE_INSTALL_FULL_LIBDIR@
   includedir=@CMAKE_INSTALL_FULL_INCLUDEDIR@

   Name: GNU Pth
   Description: GNU Portable Threads (multi-scheduler fork) - cooperative M:N threads
   URL: https://www.gnu.org/software/pth/
   Version: @PTH_VERSION_SHORT@
   Libs: -L${libdir} -lpth
   Libs.private: @PTH_PC_PRIVATE_LIBS@
   Cflags: -I${includedir}
   ```

   In the CMake install section:
   ```
   set(PTH_PC_PRIVATE_LIBS "")
   if(PTH_MP)
       set(PTH_PC_PRIVATE_LIBS "-pthread")
   endif()
   configure_file(pth.pc.in "${GEN}/pth.pc" @ONLY)
   install(FILES "${GEN}/pth.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
   ```

2. **Man pages** (CMake wasn't installing them — a real gap):
   ```
   install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pth.3" DESTINATION "${CMAKE_INSTALL_MANDIR}/man3")
   # and, under if(PTH_BUILD_PTHREAD):
   install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pthread.3" DESTINATION "${CMAKE_INSTALL_MANDIR}/man3")
   ```

3. Tidy the two stale comments that reference removed files:
   `cmake/probes/mcsc.c` ("mirrors AC_CHECK_MCSC from aclocal.m4") and
   `cmake/pth_acdef.h.cmake.in` ("Mirrors the autoheader-generated pth_acdef.h.in").

## Docs to update

- **INSTALL**: rewrite for CMake — requirements (CMake >= 3.19, C compiler, sh
  for shtool; x86_64/arm64 on Linux/FreeBSD/macOS), quick start
  (`cmake -S . -B build`, `cmake --build`, `ctest`, `cmake --install`), the
  `PTH_*` options table, mctx methods (mcsc/bctx), and how to consume the
  library (pkg-config / `-lpth -pthread` / CMake target).
- **README**: remove the "Build systems: … autoconf" bullet and the
  "BUILDING (autoconf)" block; leave the legacy section below the `LEGACY:`
  marker untouched.

## Verify

- `shtool scpp` still generates `pth_p.h`.
- A fresh `cmake -S . -B build && cmake --build build && ctest` succeeds and the
  configure step finds `shtool` and generates the headers.
- `cmake --install build --prefix /tmp/x` installs lib + `pth.h` + `pth.pc` +
  man page(s).
- CI (Linux/FreeBSD/macOS) stays green.
