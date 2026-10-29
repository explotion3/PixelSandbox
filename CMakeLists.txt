cmake_minimum_required(VERSION 4.0)
project(PixelSandbox VERSION 0.1.0 LANGUAGES CXX)
# CMake 4.0 mis-decodes the Chinese-only MSVC /showIncludes prefix on this
# machine. Preserve the real UTF-8 prefix so Ninja can track header dependencies.
if(MSVC AND CMAKE_CXX_CL_SHOWINCLUDES_PREFIX MATCHES "娉")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# A static CRT is shared consistently by runtime, bindings, extension and tests.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
option(PIXEL_BUILD_HOST "Build the Windows Godot extension" ON)
include(CTest)

# Explicitly built benchmark: real project catalog, no Godot dependency.
find_program(PIXEL_PYTHON NAMES python REQUIRED)
file(GLOB PIXEL_BENCHMARK_MATERIALS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/game/content/materials/*.tres")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/benchmark_material_catalog.hpp"
    COMMAND "${PIXEL_PYTHON}" "${PROJECT_SOURCE_DIR}/benchmarks/generate_material_catalog.py"
        "${PROJECT_SOURCE_DIR}/game" "${CMAKE_BINARY_DIR}/benchmark_material_catalog.hpp"
    DEPENDS benchmarks/generate_material_catalog.py game/content/materials/catalog.txt ${PIXEL_BENCHMARK_MATERIALS}
    VERBATIM)
add_executable(pixel_material_benchmark EXCLUDE_FROM_ALL benchmarks/material_benchmark.cpp
    "${CMAKE_BINARY_DIR}/benchmark_material_catalog.hpp")
target_include_directories(pixel_material_benchmark PRIVATE "${CMAKE_BINARY_DIR}")
target_link_libraries(pixel_material_benchmark PRIVATE pixel_runtime pixel_physics pixel_persistence)
if(MSVC)
    target_compile_options(pixel_material_benchmark PRIVATE /W4 /WX /utf-8)
endif()

add_library(pixel_runtime STATIC
    native/core/world_state.cpp
    native/core/stone_wall_scenario.cpp
    native/simulation/material_simulation.cpp
    native/simulation/liquid_simulation.cpp
    native/simulation/source_liquid_simulation.cpp
    native/simulation/material_interactions.cpp
    native/simulation/explosion_simulation.cpp
    native/simulation/electrical_simulation.cpp
    native/simulation/rigid_simulation.cpp
    native/gameplay/stone_rules.cpp
    native/content/content_registry.cpp
    native/content/reaction_table.cpp
    native/session/game_session.cpp)
target_include_directories(pixel_runtime PUBLIC native)
if(MSVC)
    target_compile_options(pixel_runtime PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
else()
    target_compile_options(pixel_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Git REQUIRED)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/box2d/CMakeLists.txt")
    message(FATAL_ERROR "Missing pinned Box2D source. Run tools/project.ps1 bootstrap.")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}/third_party/box2d" rev-parse HEAD
    OUTPUT_VARIABLE PIXEL_BOX2D_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT PIXEL_BOX2D_COMMIT STREQUAL "8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3")
    message(FATAL_ERROR "Box2D must remain at pinned v3.1.1 commit.")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}/third_party/box2d" diff --quiet HEAD -- RESULT_VARIABLE PIXEL_BOX2D_DIRTY)
if(NOT PIXEL_BOX2D_DIRTY EQUAL 0)
    message(FATAL_ERROR "Box2D has local modifications; preserve them and restore the locked source.")
endif()
set(BOX2D_SAMPLES OFF CACHE BOOL "No samples/dependency downloads" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "Project tests use the real library" FORCE)
set(BOX2D_AVX2 OFF CACHE BOOL "Windows x64 baseline" FORCE)
block(SCOPE_FOR VARIABLES)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(third_party/box2d EXCLUDE_FROM_ALL)
endblock()
add_library(pixel_physics STATIC native/physics/box2d_solver.cpp)
target_link_libraries(pixel_physics PUBLIC pixel_runtime PRIVATE box2d)
if(MSVC)
    target_compile_options(pixel_physics PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
endif()

add_library(pixel_persistence STATIC native/persistence/world_file.cpp)
target_link_libraries(pixel_persistence PUBLIC pixel_runtime)
if(MSVC)
    target_compile_options(pixel_persistence PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
endif()

if(BUILD_TESTING)
    add_executable(pixel_reaction_tests tests/reaction_tests.cpp "${CMAKE_BINARY_DIR}/benchmark_material_catalog.hpp")
    target_include_directories(pixel_reaction_tests PRIVATE "${CMAKE_BINARY_DIR}")
    target_link_libraries(pixel_reaction_tests PRIVATE pixel_runtime pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_reaction_tests PRIVATE /W4 /WX /utf-8)
    endif()
    foreach(case reaction_labels reaction_sampling reaction_required reaction_geometry reaction_regions source_phase_environment source_phase_temperature source_electricity source_blast source_corrosion source_gas source_adhesion source_adhesion_save interaction_save)
        add_test(NAME native.${case} COMMAND pixel_reaction_tests ${case} "${PROJECT_SOURCE_DIR}/game")
    endforeach()
    add_executable(pixel_combustion_tests tests/combustion_tests.cpp "${CMAKE_BINARY_DIR}/benchmark_material_catalog.hpp")
    target_include_directories(pixel_combustion_tests PRIVATE "${CMAKE_BINARY_DIR}")
    target_link_libraries(pixel_combustion_tests PRIVATE pixel_runtime pixel_physics pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_combustion_tests PRIVATE /W4 /WX /utf-8)
    endif()
    foreach(case combustion_ignition combustion_emission combustion_burnout rigid_combustion combustion_save)
        add_test(NAME native.${case} COMMAND pixel_combustion_tests ${case})
    endforeach()
    add_executable(pixel_liquid_tests tests/liquid_tests.cpp)
    target_link_libraries(pixel_liquid_tests PRIVATE pixel_runtime pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_liquid_tests PRIVATE /W4 /WX /utf-8)
    endif()
    foreach(case pressure momentum pressure_save pressure_head source_liquid_motion source_liquid_density source_liquid_save)
        add_test(NAME native.${case} COMMAND pixel_liquid_tests ${case})
    endforeach()
    add_executable(pixel_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(pixel_runtime_tests PRIVATE pixel_runtime pixel_persistence)
    foreach(case lifecycle fixed_step registry)
        add_test(NAME native.${case} COMMAND pixel_runtime_tests ${case})
    endforeach()
    add_executable(pixel_rigid_tests tests/rigid_tests.cpp)
    target_link_libraries(pixel_rigid_tests PRIVATE pixel_runtime pixel_physics pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_rigid_tests PRIVATE /W4 /WX /utf-8)
    endif()
    foreach(case rigid_motion rigid_geometry rigid_persistence rigid_thermal rigid_reactions rigid_thermal_persistence rigid_blast rigid_blast_occlusion rigid_fuses rigid_electricity rigid_electric_changes rigid_electric_save rigid_fluid rigid_fluid_layers rigid_friction rigid_pressure source_liquid_rigid)
        add_test(NAME native.${case} COMMAND pixel_rigid_tests ${case})
    endforeach()
    add_executable(pixel_stone_wall_tests tests/stone_wall_tests.cpp)
    target_link_libraries(pixel_stone_wall_tests PRIVATE pixel_runtime pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_stone_wall_tests PRIVATE /utf-8 /W4 /WX)
        target_compile_options(pixel_runtime_tests PRIVATE /utf-8 /W4 /WX)
    endif()
    foreach(case coordinates transactions persistence)
        add_test(NAME native.${case} COMMAND pixel_stone_wall_tests ${case})
    endforeach()
    add_executable(pixel_material_tests tests/material_tests.cpp)
    target_link_libraries(pixel_material_tests PRIVATE pixel_runtime pixel_persistence)
    if(MSVC)
        target_compile_options(pixel_material_tests PRIVATE /utf-8 /W4 /WX)
    endif()
    foreach(case movement liquid gas_fire reactions lab_persistence lab_tools extensibility settling liquid_wake wide_liquid transport chemistry gas_transport three_reagents mechanical adhesion slime_reactions explosion chain_reaction electricity electric_persistence electric_effects reaction_blobs blob_persistence phase_variants phase_physics phase_persistence)
        add_test(NAME native.${case} COMMAND pixel_material_tests ${case})
    endforeach()
endif()

if(PIXEL_BUILD_HOST)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The host currently requires Windows x64 MSVC. Use PIXEL_BUILD_HOST=OFF for runtime-only builds.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "19.44.35227.0")
        message(FATAL_ERROR "Locked compiler: MSVC 19.44.35227.0 (tools 14.44.35207). Use tools/project.ps1.")
    endif()
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}/third_party/godot-cpp" rev-parse HEAD
        OUTPUT_VARIABLE GODOTCPP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT GODOTCPP_COMMIT STREQUAL "507ed9d840c01a3c5b2a39af8bb4000bfac30bf5")
        message(FATAL_ERROR "godot-cpp revision mismatch. Run tools/project.ps1 bootstrap.")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}/third_party/godot-cpp" diff --quiet HEAD --
        RESULT_VARIABLE GODOTCPP_DIRTY)
    if(NOT GODOTCPP_DIRTY EQUAL 0)
        message(FATAL_ERROR "godot-cpp has local modifications; preserve them and restore the locked source before building.")
    endif()
    set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Locked target API" FORCE)
    set(GODOTCPP_TARGET "template_debug" CACHE STRING "Binding target")
    set(GODOTCPP_PRECISION single CACHE STRING "Locked precision" FORCE)
    set(GODOTCPP_USE_STATIC_CPP ON CACHE BOOL "Static CRT" FORCE)
    set(GODOTCPP_USE_HOT_RELOAD OFF CACHE BOOL "Restart after native builds" FORCE)
    set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "External headers" FORCE)
    set(GODOTCPP_BUILD_PROFILE "${PROJECT_SOURCE_DIR}/tools/godot-cpp-profile.json" CACHE FILEPATH "Required host API" FORCE)
    add_subdirectory(third_party/godot-cpp EXCLUDE_FROM_ALL)
    add_library(pixel_host SHARED
        native/godot_host/register_types.cpp
        native/godot_host/material_definition_resource.cpp
        native/godot_host/session_node.cpp
        native/godot_host/world_canvas.cpp
        native/godot_host/sandbox_app.cpp)
    target_sources(pixel_host PRIVATE tests/godot_smoke.cpp)
    target_link_libraries(pixel_host PRIVATE pixel_runtime pixel_persistence pixel_physics godot::cpp)
    target_compile_options(pixel_host PRIVATE /W4 /utf-8 /permissive- /EHsc)
    set_target_properties(pixel_host PROPERTIES
        PREFIX ""
        OUTPUT_NAME "pixel_host.$<IF:$<CONFIG:Release>,release,debug>"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/game/bin"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
    find_program(PIXEL_GODOT NAMES Godot_v4.7.2-stable_win64_console.exe
        PATHS "${PROJECT_SOURCE_DIR}/tools/_cache/godot-4.7.2-stable" NO_DEFAULT_PATH)
    if(BUILD_TESTING AND PIXEL_GODOT)
        add_test(NAME host.integration COMMAND "${CMAKE_COMMAND}"
            "-DPIXEL_GODOT=${PIXEL_GODOT}" "-DPIXEL_GAME=${PROJECT_SOURCE_DIR}/game"
            -P "${PROJECT_SOURCE_DIR}/tests/run_host_smoke.cmake")
        set_tests_properties(host.integration PROPERTIES TIMEOUT 150)
    endif()
endif()
