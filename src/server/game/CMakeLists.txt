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

CU_RUN_HOOK(BEFORE_GAME_LIBRARY)

CollectSourceFiles(
  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE_SOURCES
  # Exclude
  ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders)

if (USE_COREPCH)
  set(PRIVATE_PCH_HEADER PrecompiledHeaders/gamePCH.h)
endif ()

# Group sources
GroupSources(${CMAKE_CURRENT_SOURCE_DIR})

add_library(game-interface INTERFACE)

CollectIncludeDirectories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_INCLUDES
  # Exclude
  ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders)

target_include_directories(game-interface
  INTERFACE
    ${PUBLIC_INCLUDES})

target_link_libraries(game-interface
  INTERFACE
    shared)

add_library(game STATIC
  ${PRIVATE_SOURCES})

add_dependencies(game revision.h)

target_include_directories(game
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR})

# Forge: where the training host finds its Python learner unless AnimusForge.Learner.WorkDir says otherwise.
target_compile_definitions(game
  PRIVATE
    FORGE_PYTHON_DIR="${CMAKE_SOURCE_DIR}/apps/forge/python")

# zstd compresses the forge's baked ground-probe tables (ProbeBake). The static archive first, so the worldserver
# needs no libzstd at run time.
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES libzstd.a zstd REQUIRED)

target_link_libraries(game
  PRIVATE
    acore-core-interface
    ${ZSTD_LIBRARY}
  PUBLIC
    game-interface
    libsidecar)

target_include_directories(game
  PRIVATE
    ${ZSTD_INCLUDE_DIR})

set_target_properties(game
  PROPERTIES
    FOLDER
      "server")

# Generate precompiled header
if (USE_COREPCH)
  add_cxx_pch(game ${PRIVATE_PCH_HEADER})
endif ()

CU_RUN_HOOK(AFTER_GAME_LIBRARY)

# The forge's device code: Animus/Gpu/Device/*.hip into libforge-gpu.so, installed beside the worldserver, which
# loads it at run time (Animus/Gpu/GpuRuntime) and only when there is a GPU to use. It is compiled with hipcc but
# linked against no HIP runtime: its HIP symbols come from the runtime the learner's torch ships, loaded by the
# worldserver first, because device memory can only be shared between processes on the same HIP runtime. A machine
# without hipcc builds without it, and the forge runs everything on the CPU.
find_program(FORGE_HIPCC hipcc)
set(FORGE_GPU_ARCHS "gfx1100" CACHE STRING "GPU architectures libforge-gpu.so is compiled for (a ;-list)")
if (FORGE_HIPCC)
  file(GLOB FORGE_GPU_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Animus/Gpu/Device/*.hip)
  set(FORGE_GPU_ARCH_FLAGS)
  foreach(arch ${FORGE_GPU_ARCHS})
    list(APPEND FORGE_GPU_ARCH_FLAGS --offload-arch=${arch})
  endforeach()
  set(FORGE_GPU_OBJECTS)
  foreach(source ${FORGE_GPU_SOURCES})
    get_filename_component(name ${source} NAME_WE)
    set(object ${CMAKE_CURRENT_BINARY_DIR}/forge-gpu/${name}.o)
    add_custom_command(
      OUTPUT ${object}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/forge-gpu
      COMMAND ${FORGE_HIPCC} ${FORGE_GPU_ARCH_FLAGS} -O3 -fPIC -std=c++20 -fvisibility=hidden
        -I${CMAKE_CURRENT_SOURCE_DIR}/Animus/Gpu -c ${source} -o ${object}
      DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/Animus/Gpu/DeviceApi.h
      COMMENT "hipcc Animus/Gpu/Device/${name}.hip"
      VERBATIM)
    list(APPEND FORGE_GPU_OBJECTS ${object})
  endforeach()
  set_source_files_properties(${FORGE_GPU_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_library(forge-gpu SHARED ${FORGE_GPU_OBJECTS})
  set_target_properties(forge-gpu PROPERTIES LINKER_LANGUAGE CXX FOLDER "server")
  # The worldserver's own build brings the library along.
  add_dependencies(game forge-gpu)
  install(TARGETS forge-gpu LIBRARY DESTINATION bin)
  message(STATUS "Forge device library: ${FORGE_HIPCC}, for ${FORGE_GPU_ARCHS}")
else()
  message(STATUS "Forge device library: no hipcc, built without it")
endif()
