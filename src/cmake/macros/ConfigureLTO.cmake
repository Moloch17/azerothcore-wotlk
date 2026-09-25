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

# Forge: link-time optimisation for every non-Debug configuration, applied globally so the game
# library, the scripts and the worldserver are optimised as one unit. Included after the compiler
# settings because the probe needs the toolchain's own archiver: clang's bitcode objects go through
# llvm-ar/llvm-ranlib (the llvm package) and its linker is lld, gcc's through gcc-ar/gcc-ranlib.
# A syntax-check tree never links, so a toolchain without them only loses the optimisation.
if(WITH_LTO)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    string(REGEX MATCH "^[0-9]+" FORGE_CLANG_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
    find_program(FORGE_LLVM_AR NAMES llvm-ar-${FORGE_CLANG_MAJOR} llvm-ar)
    find_program(FORGE_LLVM_RANLIB NAMES llvm-ranlib-${FORGE_CLANG_MAJOR} llvm-ranlib)
    find_program(FORGE_LLD NAMES ld.lld-${FORGE_CLANG_MAJOR} ld.lld)
    if(FORGE_LLVM_AR AND FORGE_LLVM_RANLIB AND FORGE_LLD)
      set(CMAKE_AR "${FORGE_LLVM_AR}" CACHE FILEPATH "Archiver" FORCE)
      set(CMAKE_RANLIB "${FORGE_LLVM_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
      set(CMAKE_CXX_COMPILER_AR "${FORGE_LLVM_AR}" CACHE FILEPATH "LTO archiver" FORCE)
      set(CMAKE_CXX_COMPILER_RANLIB "${FORGE_LLVM_RANLIB}" CACHE FILEPATH "LTO ranlib" FORCE)
      set(CMAKE_C_COMPILER_AR "${FORGE_LLVM_AR}" CACHE FILEPATH "LTO archiver" FORCE)
      set(CMAKE_C_COMPILER_RANLIB "${FORGE_LLVM_RANLIB}" CACHE FILEPATH "LTO ranlib" FORCE)
      target_link_options(acore-compile-option-interface INTERFACE -fuse-ld=lld)
      set(FORGE_LTO_TOOLCHAIN_OK TRUE)
    else()
      message(STATUS "Forge: link-time optimisation skipped, clang needs llvm-ar, llvm-ranlib and ld.lld (apt install llvm lld)")
    endif()
  else()
    set(FORGE_LTO_TOOLCHAIN_OK TRUE)
  endif()

  if(FORGE_LTO_TOOLCHAIN_OK)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FORGE_IPO_SUPPORTED OUTPUT FORGE_IPO_MESSAGE LANGUAGES CXX C)
    if(FORGE_IPO_SUPPORTED)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
      message(STATUS "Forge: link-time optimisation enabled for non-Debug configurations")
    else()
      message(STATUS "Forge: link-time optimisation not supported by this toolchain: ${FORGE_IPO_MESSAGE}")
    endif()
  endif()
endif()
