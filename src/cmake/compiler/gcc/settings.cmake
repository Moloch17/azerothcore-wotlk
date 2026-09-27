#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#

set(GCC_EXPECTED_VERSION 8.0.0)

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS GCC_EXPECTED_VERSION)
  message(FATAL_ERROR "GCC: This project requires version ${GCC_EXPECTED_VERSION} to build but found ${CMAKE_CXX_COMPILER_VERSION}")
else()
  message(STATUS "GCC: Minimum version required is ${GCC_EXPECTED_VERSION}, found ${CMAKE_CXX_COMPILER_VERSION} - ok!")
endif()

if(PLATFORM EQUAL 32)
  # Required on 32-bit systems to enable SSE2 (standard on x64)
  target_compile_options(acore-compile-option-interface
    INTERFACE
      -msse2
      -mfpmath=sse)
endif()

if(ACORE_SYSTEM_PROCESSOR MATCHES "x86|amd64")
  target_compile_definitions(acore-compile-option-interface
    INTERFACE
      -DHAVE_SSE2
      -D__SSE2__)

  message(STATUS "GCC: SFMT enabled, SSE2 flags forced")
endif()

# Forge: the sim host is compiled for the machine it runs on. -O3 in every non-Debug configuration
# (RelWithDebInfo's -O2 is overridden, the option comes later on the command line), the native
# instruction set of the machine building it, and no semantic interposition so
# LTO can inline across the shared boundaries. 32-bit and non-x86 hosts keep the stock flags.
if(PLATFORM EQUAL 64 AND ACORE_SYSTEM_PROCESSOR MATCHES "x86|amd64")
  include(CheckCXXCompilerFlag)
  # The instruction set is the machine's own, found by asking the CPU (-march=native): every cluster machine builds
  # for itself, a Xeon without AVX2 gets none and a Zen 5 gets AVX-512. It used to be -march=znver5 whenever the
  # compiler knew Zen 5, which is a test of the compiler, not of the CPU: on an Intel or older AMD machine that
  # binary dies on its first AVX-512 instruction. Only the scheduling model is added by hand, for a Zen 5 whose
  # compiler resolves native to a generic model: znver5 when it knows it, else znver4, the closest it does.
  set(FORGE_ARCH_FLAGS -march=native)
  if(EXISTS "/proc/cpuinfo")
    file(STRINGS "/proc/cpuinfo" FORGE_CPU_VENDOR REGEX "^vendor_id" LIMIT_COUNT 1)
    file(STRINGS "/proc/cpuinfo" FORGE_CPU_FAMILY REGEX "^cpu family" LIMIT_COUNT 1)
    if(FORGE_CPU_VENDOR MATCHES "AuthenticAMD" AND FORGE_CPU_FAMILY MATCHES ":[ \t]*26$")
      check_cxx_compiler_flag("-mtune=znver5" FORGE_HAVE_ZNVER5_TUNE)
      check_cxx_compiler_flag("-mtune=znver4" FORGE_HAVE_ZNVER4_TUNE)
      if(FORGE_HAVE_ZNVER5_TUNE)
        list(APPEND FORGE_ARCH_FLAGS -mtune=znver5)
      elseif(FORGE_HAVE_ZNVER4_TUNE)
        list(APPEND FORGE_ARCH_FLAGS -mtune=znver4)
      endif()
    endif()
  endif()
  target_compile_options(acore-compile-option-interface
    INTERFACE
      $<$<NOT:$<CONFIG:Debug>>:-O3>
      ${FORGE_ARCH_FLAGS}
      -fno-semantic-interposition)
  message(STATUS "Forge: native tuning ${FORGE_ARCH_FLAGS}, -O3 for non-Debug configurations")
endif()

# Forge: profile-guided optimisation. Pass 1 (FORGE_PGO=generate) builds an instrumented
# worldserver that writes its profile into FORGE_PGO_DIR while it runs a representative training
# sweep; pass 2 (FORGE_PGO=use) rebuilds with that profile. The profile format differs per compiler,
# so both passes must use the same one.
if(FORGE_PGO STREQUAL "generate")
  target_compile_options(acore-compile-option-interface INTERFACE -fprofile-generate=${FORGE_PGO_DIR} -fprofile-update=atomic)
  target_link_options(acore-compile-option-interface INTERFACE -fprofile-generate=${FORGE_PGO_DIR})
  message(STATUS "Forge: PGO instrumented build, profile written to ${FORGE_PGO_DIR}")
elseif(FORGE_PGO STREQUAL "use")
  target_compile_options(acore-compile-option-interface INTERFACE -fprofile-use=${FORGE_PGO_DIR} -fprofile-correction -Wno-missing-profile)
  target_link_options(acore-compile-option-interface INTERFACE -fprofile-use=${FORGE_PGO_DIR})
  message(STATUS "Forge: PGO optimised build from ${FORGE_PGO_DIR}")
endif()

if( WITH_WARNINGS )
  target_compile_options(acore-warning-interface
  INTERFACE
    -W
    -Wall
    -Wextra
    -Winit-self
    -Winvalid-pch
    -Wfatal-errors
    -Woverloaded-virtual)
  message(STATUS "GCC: All warnings enabled")
endif()

if( WITH_COREDEBUG )
  target_compile_options(acore-compile-option-interface
  INTERFACE
    -g3)
  message(STATUS "GCC: Debug-flags set (-g3)")
endif()

if(BUILD_SHARED_LIBS)
  target_compile_options(acore-compile-option-interface
    INTERFACE
      -fPIC
      -Wno-attributes)

  target_compile_options(acore-hidden-symbols-interface
    INTERFACE
      -fvisibility=hidden)

  # Should break the build when there are ACORE_*_API macros missing
  # but it complains about missing references in precompiled headers.
  # set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--no-undefined")
  # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,--no-undefined")

  message(STATUS "GCC: Enabled shared linking")
endif()
