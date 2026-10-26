cmake_minimum_required(VERSION 3.28.1)
project(mpdecimal LANGUAGES C)

# mpdecimal ships autotools, not CMake, and mpdecimal.h itself is generated:
# upstream's own Makefile.vc produces it for MSVC by copying the pre-baked
# 32/64-bit header instead of running autoconf's mpdecimal.h.in substitution.
# Mirror that here for the 64-bit configuration.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/libmpdec/mpdecimal64vc.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/libmpdec/mpdecimal.h"
    COPYONLY
)

# Matches libmpdec/Makefile.vc's OBJS list for the "ansi64" configuration
# (CONFIG_64 + ANSI, i.e. no vcdiv64.asm / MASM intrinsics). bench.c and
# bench_full.c are excluded: they are standalone benchmark tools with their
# own main(), not part of the library.
set(LIBMPDEC_SOURCES
    libmpdec/basearith.c
    libmpdec/constants.c
    libmpdec/context.c
    libmpdec/convolute.c
    libmpdec/crt.c
    libmpdec/difradix2.c
    libmpdec/fnt.c
    libmpdec/fourstep.c
    libmpdec/io.c
    libmpdec/mpalloc.c
    libmpdec/mpdecimal.c
    libmpdec/mpsignal.c
    libmpdec/numbertheory.c
    libmpdec/sixstep.c
    libmpdec/transpose.c
)

add_library(libmpdec STATIC ${LIBMPDEC_SOURCES})

target_compile_definitions(libmpdec PRIVATE
    CONFIG_64
    ANSI
    _CRT_SECURE_NO_WARNINGS
)

# mpdecimal.h's IMPORTEXPORT macro decorates every declaration
# __declspec(dllimport) as soon as _DLL is defined (i.e. under vcpkg's
# dynamic-CRT triplets), which MSVC then refuses to also define in the
# same translation unit. BUILD_LIBMPDEC switches it to dllexport instead,
# which is what upstream's own shared-library recipe defines for the
# same reason. This must be PUBLIC: mpdecimal.h is a consumer-visible
# header, so any target that includes it needs the same definition, or
# it decorates the consumer's calls dllimport and the linker goes
# looking for __imp_ thunks that this plain static .lib doesn't contain.
# ...but only where __declspec exists. Every reason above is MSVC-specific: `_DLL` is
# an MSVC predefine, and `__imp_` thunks are a PE/COFF concern. Apple and Android clang
# reject `__declspec` outright unless -fdeclspec/-fms-extensions is passed, so defining
# BUILD_LIBMPDEC there turns mpdecimal.h's every declaration into a hard error:
#   error: '__declspec' attributes are not enabled
# mpdecimal.h's own guard is `#if defined(BUILD_LIBMPDEC)` with no platform test, so the
# gate has to live here. Leaving it undefined lets IMPORTEXPORT stay empty, which is the
# correct decoration for a static library on ELF/Mach-O.
if (MSVC)
  target_compile_definitions(libmpdec PUBLIC
      BUILD_LIBMPDEC
  )
endif ()

target_include_directories(libmpdec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libmpdec>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS libmpdec
    EXPORT mpdecimal-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES libmpdec/mpdecimal.h DESTINATION include)

install(EXPORT mpdecimal-targets
    FILE mpdecimal-config.cmake
    DESTINATION share/mpdecimal
)
