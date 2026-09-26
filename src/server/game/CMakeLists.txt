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
