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

if ((USE_COREPCH OR USE_SCRIPTPCH) AND (CMAKE_C_COMPILER_LAUNCHER STREQUAL "ccache" OR CMAKE_CXX_COMPILER_LAUNCHER STREQUAL "ccache"))
  message(STATUS "Clang: disable pch timestamp when ccache and pch enabled")
  # TODO: for ccache https://github.com/ccache/ccache/issues/539
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Xclang -fno-pch-timestamp")
endif()

set(CLANG_EXPECTED_VERSION 10.0.0)

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS CLANG_EXPECTED_VERSION)
  message(FATAL_ERROR "Clang: AzerothCore requires version ${CLANG_EXPECTED_VERSION} to build but found ${CMAKE_CXX_COMPILER_VERSION}")
else()
  message(STATUS "Clang: Minimum version required is ${CLANG_EXPECTED_VERSION}, found ${CMAKE_CXX_COMPILER_VERSION} - ok!")
endif()

# This tests for a bug in clang-7 that causes linkage to fail for 64-bit from_chars (in some configurations)
# If the clang requirement is bumped to >= clang-8, you can remove this check, as well as
# the associated ifdef block in src/common/Utilities/StringConvert.h
include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
#include <charconv>
#include <cstdint>
int main()
{
    uint64_t n;
    char const c[] = \"0\";
    std::from_chars(c, c+1, n);
    return static_cast<int>(n);
}
" CLANG_HAVE_PROPER_CHARCONV)

# Forge: the sim host is compiled for the machine it runs on. -O3 in every non-Debug configuration
# (RelWithDebInfo's -O2 is overridden, the option comes later on the command line), the native
# instruction set with the newest Zen tuning the compiler knows, and no semantic interposition so
# LTO can inline across the shared boundaries. 32-bit and non-x86 hosts keep the stock flags.
if(PLATFORM EQUAL 64 AND ACORE_SYSTEM_PROCESSOR MATCHES "x86|amd64")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=znver5" FORGE_HAVE_ZNVER5)
  check_cxx_compiler_flag("-mtune=znver4" FORGE_HAVE_ZNVER4_TUNE)
  if(FORGE_HAVE_ZNVER5)
    set(FORGE_ARCH_FLAGS -march=znver5)
  elseif(FORGE_HAVE_ZNVER4_TUNE)
    # The compiler does not know Zen 5: -march=native still enables every instruction the CPU has
    # (AVX-512 included), and the Zen 4 model is the closest scheduling description it can use.
    set(FORGE_ARCH_FLAGS -march=native -mtune=znver4)
  else()
    set(FORGE_ARCH_FLAGS -march=native)
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
  target_compile_options(acore-compile-option-interface INTERFACE -fprofile-instr-generate=${FORGE_PGO_DIR}/worldserver-%m.profraw)
  target_link_options(acore-compile-option-interface INTERFACE -fprofile-instr-generate=${FORGE_PGO_DIR}/worldserver-%m.profraw)
  message(STATUS "Forge: PGO instrumented build, raw profiles written to ${FORGE_PGO_DIR} (merge with llvm-profdata merge -o ${FORGE_PGO_DIR}/worldserver.profdata ${FORGE_PGO_DIR}/*.profraw)")
elseif(FORGE_PGO STREQUAL "use")
  target_compile_options(acore-compile-option-interface INTERFACE -fprofile-instr-use=${FORGE_PGO_DIR}/worldserver.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
  message(STATUS "Forge: PGO optimised build from ${FORGE_PGO_DIR}/worldserver.profdata")
endif()

if(WITH_WARNINGS)
  target_compile_options(acore-warning-interface
    INTERFACE
      -W
      -Wall
      -Wextra
      -Winit-self
      -Wfatal-errors
      -Wno-mismatched-tags
      -Woverloaded-virtual)
  message(STATUS "Clang: All warnings enabled")
endif()

if(WITH_COREDEBUG)
  target_compile_options(acore-compile-option-interface
    INTERFACE
      -g3)
  message(STATUS "Clang: Debug-flags set (-g3)")
endif()

if(MSAN)
    target_compile_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=memory
            -fsanitize-memory-track-origins
            -mllvm
            -msan-keep-going=1)

    target_link_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=memory
            -fsanitize-memory-track-origins)

    message(STATUS "Clang: Enabled Memory Sanitizer MSan")
endif()

if(UBSAN)
    target_compile_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=undefined)

    target_link_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=undefined)

    message(STATUS "Clang: Enabled Undefined Behavior Sanitizer UBSan")
endif()

if(TSAN)
    target_compile_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=thread)

    target_link_options(acore-compile-option-interface
            INTERFACE
            -fno-omit-frame-pointer
            -fsanitize=thread)

    message(STATUS "Clang: Enabled Thread Sanitizer TSan")
endif()

# -Wno-narrowing needed to suppress a warning in g3d
# -Wno-deprecated-register is needed to suppress gsoap warnings on Unix systems.
target_compile_options(acore-compile-option-interface
  INTERFACE
    -Wno-narrowing
    -Wno-deprecated-register)

if(BUILD_SHARED_LIBS)
    # -fPIC is needed to allow static linking in shared libs.
    # -fvisibility=hidden sets the default visibility to hidden to prevent exporting of all symbols.
    target_compile_options(acore-compile-option-interface
      INTERFACE
        -fPIC)

    target_compile_options(acore-hidden-symbols-interface
      INTERFACE
        -fvisibility=hidden)

    # --no-undefined to throw errors when there are undefined symbols
    # (caused through missing ACORE_*_API macros).
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --no-undefined")

    message(STATUS "Clang: Disallow undefined symbols")
endif()
