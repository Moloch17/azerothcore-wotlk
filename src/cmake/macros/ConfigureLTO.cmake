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

      # And again as ordinary variables, which is what the archive rules actually read. project() loads
      # the compiler detection CMake stored in the build tree, and that file does a plain set() of these:
      # a normal variable shadows the cache entry above for the rest of the configure. A build tree
      # configured before the toolchain had llvm-ar keeps "CMAKE_CXX_COMPILER_AR-NOTFOUND" in that file
      # for as long as the compiler itself does not change, and CMake then writes that literal string
      # into every static library's link.txt, where it fails as "Error running link command: No such file
      # or directory". Setting them here, before any add_subdirectory, fixes a stale build tree in place.
      set(CMAKE_CXX_COMPILER_AR "${FORGE_LLVM_AR}")
      set(CMAKE_CXX_COMPILER_RANLIB "${FORGE_LLVM_RANLIB}")
      set(CMAKE_C_COMPILER_AR "${FORGE_LLVM_AR}")
      set(CMAKE_C_COMPILER_RANLIB "${FORGE_LLVM_RANLIB}")
      target_link_options(acore-compile-option-interface INTERFACE -fuse-ld=lld)
      set(FORGE_LTO_TOOLCHAIN_OK TRUE)
    else()
      message(STATUS "Forge: link-time optimisation skipped, clang needs llvm-ar, llvm-ranlib and ld.lld (apt install llvm lld)")
    endif()
  else()
    set(FORGE_LTO_TOOLCHAIN_OK TRUE)
  endif()

  # check_ipo_supported cannot answer for clang here. It builds a project of its own through the project
  # form of try_compile, which forwards a fixed set of variables and not the archiver: its test linked
  # with "CMAKE_CXX_COMPILER_AR-NOTFOUND" and it reported the toolchain as unable to do LTO while the real
  # build was able to do it, so the optimisation was silently absent from every binary.
  # CMAKE_TRY_COMPILE_PLATFORM_VARIABLES does not help, because that applies to the source-file form.
  #
  # For clang the check above is the real one: llvm-ar, llvm-ranlib and ld.lld either exist or they do
  # not, and if LTO were still broken the link would say so loudly rather than quietly produce a slower
  # binary. The probe stays for any other compiler, where nothing had to be found by hand.
  if(FORGE_LTO_TOOLCHAIN_OK AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(FORGE_IPO_SUPPORTED TRUE)
  elseif(FORGE_LTO_TOOLCHAIN_OK)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FORGE_IPO_SUPPORTED OUTPUT FORGE_IPO_MESSAGE LANGUAGES CXX C)
    if(NOT FORGE_IPO_SUPPORTED)
      message(STATUS "Forge: link-time optimisation not supported by this toolchain: ${FORGE_IPO_MESSAGE}")
    endif()
  endif()

  if(FORGE_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    message(STATUS "Forge: link-time optimisation enabled for non-Debug configurations")
  endif()
endif()
