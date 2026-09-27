cmake_minimum_required(VERSION 3.20)
project(emergent_native LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# EMERGENT intentionally has no home-grown replacements for mature engine infrastructure.
# Provide pinned upstream source trees or enable FetchContent on an internet-connected build host.
option(EMERGENT_FETCH_DEPS "Fetch pinned upstream open-source dependencies" OFF)
option(EMERGENT_REQUIRE_UPSTREAM "Fail the native build when a selected upstream subsystem is unavailable" ON)

set(EMERGENT_JOLT_DIR "" CACHE PATH "Pinned JoltPhysics source tree")
set(EMERGENT_MESHOPTIMIZER_DIR "" CACHE PATH "Pinned meshoptimizer source tree")
set(EMERGENT_RECAST_DIR "" CACHE PATH "Pinned RecastNavigation source tree")
set(EMERGENT_VOLK_DIR "" CACHE PATH "Pinned Volk source tree")
set(EMERGENT_VMA_DIR "" CACHE PATH "Pinned VulkanMemoryAllocator source tree")
set(EMERGENT_FLECS_DIR "" CACHE PATH "Pinned Flecs source tree")
set(EMERGENT_OZZ_DIR "" CACHE PATH "Pinned ozz-animation source tree")
set(EMERGENT_MINIAUDIO_DIR "" CACHE PATH "Pinned miniaudio source tree")
set(EMERGENT_ZSTD_DIR "" CACHE PATH "Pinned zstd source tree")
set(EMERGENT_KTX_DIR "" CACHE PATH "Pinned Khronos KTX-Software source tree")
set(EMERGENT_SLANG_DIR "" CACHE PATH "Pinned Slang source tree")
set(EMERGENT_TRACY_DIR "" CACHE PATH "Pinned Tracy source tree")
set(EMERGENT_THE_FORGE_DIR "" CACHE PATH "Pinned The Forge source tree")

include(FetchContent)
if(EMERGENT_FETCH_DEPS)
  FetchContent_Declare(jolt GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git GIT_TAG v5.6.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(meshoptimizer GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git GIT_TAG v1.2 GIT_SHALLOW TRUE)
  FetchContent_Declare(recastnavigation GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git GIT_TAG v1.6.0 GIT_SHALLOW TRUE)
  # Volk tags its releases `vulkan-sdk-<version>`, e.g. vulkan-sdk-1.4.328.0.
  # A bare `1.4.328` is not a real tag and makes FetchContent fail outright.
  FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.328.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.3.0 GIT_SHALLOW TRUE)
  FetchContent_Declare(flecs GIT_REPOSITORY https://github.com/SanderMertens/flecs.git GIT_TAG v4.0.5 GIT_SHALLOW TRUE)
  FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.25 GIT_SHALLOW TRUE)
  FetchContent_Declare(ozz_animation GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git GIT_TAG 0.17.0 GIT_SHALLOW TRUE)
  set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ozz_build_samples OFF CACHE BOOL "" FORCE)
  set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
  set(ozz_build_tests OFF CACHE BOOL "" FORCE)
  set(ozz_build_tools OFF CACHE BOOL "" FORCE)
  set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
  set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
  set(VMA_STATIC_VULKAN_FUNCTIONS OFF CACHE BOOL "" FORCE)
  set(VMA_DYNAMIC_VULKAN_FUNCTIONS ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(jolt meshoptimizer recastnavigation volk vma flecs miniaudio ozz_animation)
endif()

# ---------------------------------------------------------------------------
# Jolt Physics — built and tested for real
# ---------------------------------------------------------------------------
# Jolt is the one native subsystem that is genuinely wired up, compiled and
# covered by a passing test (native/tests/physics_test.cpp). It is pulled in
# independently of the full engine target so `ctest` exercises real physics even
# on a host that has no Vulkan SDK and none of the other upstream libraries.
#
# Required Jolt options, and why:
#   JPH_USE_DX12/VK/MTL/CPU_COMPUTE=OFF  Hair and soft-body compute shaders are
#       compiled with glslc at build time. We do not use them, and requiring a
#       shader compiler to build a rigid-body library is a needless dependency.
#   INTERPROCEDURAL_OPTIMIZATION=OFF     LTO is a poor trade on small builds.
#   ENABLE_ALL_WARNINGS=OFF              Jolt builds with -Wall -Werror, and its
#       own DebugRendererPlayback.cpp trips -Werror=uninitialized on GCC 11
#       (a known upstream false positive). Warnings still print; they just do
#       not fail a third-party build.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(BUILD_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_CROSS_PLATFORM_DLL OFF CACHE BOOL "" FORCE)
set(FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)

if(NOT TARGET Jolt)
  if(EMERGENT_JOLT_DIR AND EXISTS "${EMERGENT_JOLT_DIR}/Build/CMakeLists.txt")
    message(STATUS "EMERGENT: using pinned Jolt tree at ${EMERGENT_JOLT_DIR}")
    # Add Build/, not the repository root: Jolt has no root-level CMakeLists.txt,
    # so adding the root fails with "does not contain a CMakeLists.txt file".
    add_subdirectory("${EMERGENT_JOLT_DIR}/Build" "${CMAKE_BINARY_DIR}/jolt")
  else()
    # Fetch the pinned release. This is the documented, supported way to get a
    # reproducible upstream dependency, and it is what CI uses.
    #
    # Jolt's CMake entry point lives in Build/CMakeLists.txt, not at the repo
    # root, so the source is populated manually rather than with
    # FetchContent_MakeAvailable (which would find no root CMakeLists.txt and
    # silently define no targets).
    message(STATUS "EMERGENT: fetching pinned Jolt Physics v5.6.0")
    FetchContent_Declare(jolt
      GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
      GIT_TAG v5.6.0
      GIT_SHALLOW TRUE
      EXCLUDE_FROM_ALL)
    FetchContent_GetProperties(jolt)
    if(NOT jolt_POPULATED)
      FetchContent_Populate(jolt)
    endif()
    add_subdirectory("${jolt_SOURCE_DIR}/Build" "${CMAKE_BINARY_DIR}/jolt" EXCLUDE_FROM_ALL)
  endif()
endif()

if(NOT TARGET Jolt)
  message(FATAL_ERROR "EMERGENT could not obtain Jolt Physics. Set -DEMERGENT_JOLT_DIR=<path> or allow network access for FetchContent.")
endif()

# Physics library + behaviour test. Deliberately independent of the full
# engine target below, so this part of the native tree stays buildable and
# verifiable on its own.
add_library(emergent_physics STATIC native/src/jolt_physics.cpp)
target_include_directories(emergent_physics PUBLIC native/include)
target_link_libraries(emergent_physics PUBLIC Jolt)

add_executable(emergent_physics_test native/tests/physics_test.cpp)
target_link_libraries(emergent_physics_test PRIVATE emergent_physics)
if(UNIX)
  target_compile_options(emergent_physics PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  target_compile_options(emergent_physics_test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(emergent_physics PRIVATE Threads::Threads)
endif()

add_test(NAME emergent_physics COMMAND emergent_physics_test)

# ---------------------------------------------------------------------------
# Asset packs — Zstandard, built and tested for real
# ---------------------------------------------------------------------------
# EMERGENT ships content as compressed packs. Zstandard is used for the
# container, and it is also what proves the container is undamaged: every
# payload and the index are complete zstd frames, so a corrupted or truncated
# pack fails to decompress instead of loading garbage.
#
# Only the library is built. zstd's CLI programs, tests and dictionary
# builder are separate executables EMERGENT never invokes, and each one costs
# another link on a single-core build host.
if(NOT TARGET libzstd_static)
  if(EMERGENT_ZSTD_DIR AND EXISTS "${EMERGENT_ZSTD_DIR}/build/cmake/CMakeLists.txt")
    message(STATUS "EMERGENT: using pinned Zstandard tree at ${EMERGENT_ZSTD_DIR}")
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    # The static target is the only one that works without an install step, and
    # it is what the pack format and the engine both link.
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${EMERGENT_ZSTD_DIR}/build/cmake" "${CMAKE_BINARY_DIR}/zstd")
  endif()
endif()

if(TARGET libzstd_static)
  add_library(emergent_assets STATIC native/src/asset_pack.cpp)
  target_include_directories(emergent_assets PUBLIC native/include)
  target_link_libraries(emergent_assets PUBLIC libzstd_static)

  add_executable(emergent_asset_pack_test native/tests/asset_pack_test.cpp)
  target_link_libraries(emergent_asset_pack_test PRIVATE emergent_assets)
  if(UNIX)
    target_compile_options(emergent_assets PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    target_compile_options(emergent_asset_pack_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  add_test(NAME emergent_asset_pack COMMAND emergent_asset_pack_test)
else()
  message(STATUS "EMERGENT: no Zstandard tree supplied; compressed asset packs are not built.")
endif()

# ---------------------------------------------------------------------------
# Profiling — always-on zone accounting, optional Tracy capture
# ---------------------------------------------------------------------------
# Zone accounting is unconditional: it is cheap, it allocates nothing after
# start-up, and it writes a JSON profile a CI job can assert on.
#
# Tracy is off by default. Its client streams to a running Tracy server, which
# does not exist in a build container, so switching it on here would produce a
# dependency that cannot be verified. Turn it on where a developer is actually
# looking at a capture.
option(EMERGENT_ENABLE_TRACY "Compile in Tracy live capture (needs a running Tracy server)" OFF)

if(NOT TARGET TracyClient)
  if(EMERGENT_TRACY_DIR AND EXISTS "${EMERGENT_TRACY_DIR}/CMakeLists.txt")
    message(STATUS "EMERGENT: using pinned Tracy tree at ${EMERGENT_TRACY_DIR}")
    # TRACY_ENABLE must be ON. With it off, TracyClient.cpp compiles to an empty
    # stub that defines none of the profiler API, so anything that calls into
    # Tracy still compiles and then fails to link on tracy::GetProfiler().
    # That is exactly the failure the CI Tracy build exists to catch.
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
    set(TRACY_CALLSTACK OFF CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE OFF CACHE BOOL "" FORCE)
    add_subdirectory("${EMERGENT_TRACY_DIR}" "${CMAKE_BINARY_DIR}/tracy")
  endif()
endif()

add_library(emergent_profiling STATIC native/src/profiling.cpp)
target_include_directories(emergent_profiling PUBLIC native/include)
if(EMERGENT_ENABLE_TRACY AND TARGET Tracy::TracyClient)
  message(STATUS "EMERGENT: Tracy live capture enabled")
  target_link_libraries(emergent_profiling PUBLIC Tracy::TracyClient)
  target_compile_definitions(emergent_profiling PUBLIC EMERGENT_WITH_TRACY=1)
elseif(EMERGENT_ENABLE_TRACY)
  message(WARNING "EMERGENT_ENABLE_TRACY=ON but no Tracy tree was supplied; zone accounting only.")
endif()
if(UNIX)
  target_compile_options(emergent_profiling PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---------------------------------------------------------------------------
# Animation — ozz-animation, built and tested for real
# ---------------------------------------------------------------------------
# EMERGENT authors its rig and its clips in code (see native/src/animation.cpp),
# so it needs the ozz *offline* builders — SkeletonBuilder and AnimationBuilder —
# on top of the runtime jobs. That is the difference between ozz being wired in
# and ozz being dead weight: without a caller the linker drops both archives.
#
# ozz's samples, how-tos, tests and asset tools are separate applications that
# pull in OpenGL/GLFW/SDL2 and their own pipelines, and cannot be built headless.
# The real switches are `ozz_build_*`; the old BUILD_* names silently did
# nothing and ozz's samples were built anyway, which is what broke configure.
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)

if(NOT TARGET ozz_animation)
  if(EMERGENT_OZZ_DIR AND EXISTS "${EMERGENT_OZZ_DIR}/CMakeLists.txt")
    message(STATUS "EMERGENT: using pinned ozz-animation tree at ${EMERGENT_OZZ_DIR}")
    add_subdirectory("${EMERGENT_OZZ_DIR}" "${CMAKE_BINARY_DIR}/ozz-animation")
  endif()
endif()

if(TARGET ozz_animation AND TARGET ozz_animation_offline)
  add_library(emergent_animation STATIC native/src/animation.cpp)
  target_include_directories(emergent_animation PUBLIC native/include)
  target_link_libraries(emergent_animation PUBLIC ozz_base ozz_animation ozz_animation_offline)

  add_executable(emergent_animation_test native/tests/animation_test.cpp)
  target_link_libraries(emergent_animation_test PRIVATE emergent_animation)
  if(UNIX)
    target_compile_options(emergent_animation PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    target_compile_options(emergent_animation_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  add_test(NAME emergent_animation COMMAND emergent_animation_test)
else()
  message(STATUS "EMERGENT: no ozz-animation tree supplied; the animation subsystem is not built.")
endif()

# The profiling test drives the physics and animation subsystems for real, so
# that "the engine reports through the profiler" is checked rather than assumed.
# It is registered here because those two targets are only defined above.
if(TARGET emergent_animation)
  add_executable(emergent_profiling_test native/tests/profiling_test.cpp)
  target_link_libraries(emergent_profiling_test PRIVATE emergent_profiling emergent_physics emergent_animation)
  if(UNIX)
    target_compile_options(emergent_profiling_test PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME emergent_profiling COMMAND emergent_profiling_test)
endif()

# Developer-supplied trees are equally first-class and keep CI/offline builds reproducible.
# Jolt's CMake entry point is the repository's Build/ directory, not its root.
# Accept either so a user can point at a clone or at the Build dir itself.
#
# Jolt defaults are tuned for a desktop development machine. Several are wrong
# for a headless CPU server build and are corrected here:
#   - JPH_USE_VK pulls in Jolt's optional Vulkan *compute* backend, which
#     precompiles HLSL shaders via glslc/dxc and hard-fails when no shader
#     compiler is installed. EMERGENT runs Jolt on the CPU, so it is disabled.
#   - DEBUG_RENDERER_IN_DEBUG_AND_RELEASE builds a debug render path EMERGENT
#     never uses.
#   - INTERPROCEDURAL_OPTIMIZATION enables LTO, which roughly doubles link time
#     for no measurable gain in a build that is not shipped to players.
if(EMERGENT_JOLT_DIR)
  set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
  set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
endif()
# Jolt was already added by the physics section above, which resolves it ahead of
# everything else. Re-adding it here would reuse the same binary directory and
# CMake rejects that, so this stays guarded.
if(NOT TARGET Jolt)
  if(EMERGENT_JOLT_DIR AND EXISTS "${EMERGENT_JOLT_DIR}/CMakeLists.txt")
    add_subdirectory("${EMERGENT_JOLT_DIR}" "${CMAKE_BINARY_DIR}/jolt")
  elseif(EMERGENT_JOLT_DIR AND EXISTS "${EMERGENT_JOLT_DIR}/Build/CMakeLists.txt")
    add_subdirectory("${EMERGENT_JOLT_DIR}/Build" "${CMAKE_BINARY_DIR}/jolt")
  endif()
endif()
if(EMERGENT_MESHOPTIMIZER_DIR AND EXISTS "${EMERGENT_MESHOPTIMIZER_DIR}/CMakeLists.txt")
  add_subdirectory("${EMERGENT_MESHOPTIMIZER_DIR}" "${CMAKE_BINARY_DIR}/meshoptimizer")
endif()
if(EMERGENT_RECAST_DIR AND EXISTS "${EMERGENT_RECAST_DIR}/CMakeLists.txt")
  set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${EMERGENT_RECAST_DIR}" "${CMAKE_BINARY_DIR}/recastnavigation")
endif()
if(EMERGENT_VOLK_DIR AND EXISTS "${EMERGENT_VOLK_DIR}/CMakeLists.txt")
  add_subdirectory("${EMERGENT_VOLK_DIR}" "${CMAKE_BINARY_DIR}/volk")
endif()
if(EMERGENT_VMA_DIR AND EXISTS "${EMERGENT_VMA_DIR}/CMakeLists.txt")
  add_subdirectory("${EMERGENT_VMA_DIR}" "${CMAKE_BINARY_DIR}/vma")
endif()
if(EMERGENT_FLECS_DIR AND EXISTS "${EMERGENT_FLECS_DIR}/CMakeLists.txt")
  add_subdirectory("${EMERGENT_FLECS_DIR}" "${CMAKE_BINARY_DIR}/flecs")
endif()
if(EMERGENT_MINIAUDIO_DIR AND EXISTS "${EMERGENT_MINIAUDIO_DIR}/CMakeLists.txt")
  add_subdirectory("${EMERGENT_MINIAUDIO_DIR}" "${CMAKE_BINARY_DIR}/miniaudio")
endif()
if(NOT TARGET ozz_animation AND EMERGENT_OZZ_DIR AND EXISTS "${EMERGENT_OZZ_DIR}/CMakeLists.txt")
  # Already added, with its build switches resolved, by the animation section
  # above. This guard only exists so a tree that arrives after that section has
  # already run is still picked up; re-adding it would reuse one binary
  # directory, which CMake rejects.
  add_subdirectory("${EMERGENT_OZZ_DIR}" "${CMAKE_BINARY_DIR}/ozz-animation")
endif()

# ---------------------------------------------------------------------------
# Frame loop
# ---------------------------------------------------------------------------
# The fixed/variable timestep loop, split into its own library rather than
# folded into the engine executable. Two reasons, both of them about being
# able to check claims: the behaviour tests link it without pulling in Vulkan,
# the render backend or the ten upstream libraries, and a build configured with
# -DEMERGENT_BUILD_ENGINE=OFF still gets a testable frame loop.
add_library(emergent_frame_loop STATIC
  native/src/frame_loop.cpp)
target_include_directories(emergent_frame_loop PUBLIC native/include)
# emergent_animation is conditional on an ozz tree being supplied, so it cannot
# be named here unconditionally. Naming it anyway makes the link fail with a
# bare `-lemergent_animation: No such file` in any configuration without ozz --
# which is exactly the configuration whose whole purpose is to build the parts
# that do not need a third-party tree. The frame loop degrades to physics and
# profiling rather than failing to link.
target_link_libraries(emergent_frame_loop PUBLIC emergent_physics emergent_profiling)
if(TARGET emergent_animation)
  target_link_libraries(emergent_frame_loop PUBLIC emergent_animation)
endif()
set_target_properties(emergent_frame_loop PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_frame_loop PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# The renderer interface, its headless implementation and the SPIR-V loader.
# Kept apart from the Vulkan renderer itself, which needs a Vulkan SDK, so
# that the validation logic a renderer depends on is testable without one.
add_library(emergent_render STATIC
  native/src/render_backend.cpp)
target_include_directories(emergent_render PUBLIC native/include)
target_link_libraries(emergent_render PUBLIC emergent_frame_loop)
set_target_properties(emergent_render PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_render PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(emergent_frame_loop_test native/tests/frame_loop_test.cpp)
target_link_libraries(emergent_frame_loop_test PRIVATE emergent_render)
if(UNIX)
  target_compile_options(emergent_frame_loop_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME emergent_frame_loop COMMAND emergent_frame_loop_test)

# ---------------------------------------------------------------------------
# The world, generated in C++
# ---------------------------------------------------------------------------
# The native renderer is no longer a debug presentation layer over a scene of
# boxes: it generates and draws the same city the JavaScript reference does,
# from the same seed, with the same integer draws. `world_gen.cpp` is a port of
# `world.mjs` and the parity test below is what makes that claim checkable
# rather than aspirational -- it compares the C++ against a fixture generated
# from the JavaScript, so a divergence in the hash functions shows up as a moved
# building rather than as a subtly different skyline nobody mentions.
add_library(emergent_world STATIC native/src/world_gen.cpp)
target_include_directories(emergent_world PUBLIC native/include)
set_target_properties(emergent_world PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_world PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(emergent_world_parity_test native/tests/world_parity_test.cpp)
target_link_libraries(emergent_world_parity_test PRIVATE emergent_world)
if(UNIX)
  target_compile_options(emergent_world_parity_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME emergent_world_parity COMMAND emergent_world_parity_test)

# ---------------------------------------------------------------------------
# The material system and the geometry emitters
# ---------------------------------------------------------------------------
# The renderer draws real geometry with real materials, not debug boxes. These
# two libraries are that world description in C++: the material table the
# shader resolves an index through, and the detail kit that fills a facade with
# windows, a cornice and roof plant. They depend on nothing upstream, so they
# build and are tested on a machine with no Vulkan, no driver and no GPU.
# The baked material table, generated from the bake descriptor and the packed
# KTX2 maps. A build step rather than a committed file, so the table and the
# textures it indexes can never disagree, and so a re-bake cannot leave a stale
# header behind.
#
# Guarded on node: a machine without it still configures and builds, and
# emergent_materials falls back to the untextured table. The alternative is a
# build that cannot run before an asset pipeline, which is worse than a world
# that is honestly grey.
find_program(EMERGENT_NODE node)
if(EMERGENT_NODE AND EXISTS "${CMAKE_SOURCE_DIR}/assets/textures/materials.mjs")
  set(EMERGENT_BAKED_SIZE 512 CACHE STRING "Texture resolution the native bake is packed at")
  # The three packs are declared outputs, not side effects. With only the header
  # declared, the command went "up to date" the moment the header was newer than
  # its sources -- so deleting 50 MB of KTX2 did not bring it back, and the
  # renderer and its test ran against a directory that was simply gone. A build
  # system cannot track what it is not told about.
  set(EMERGENT_BAKED_PACKS
    "${CMAKE_SOURCE_DIR}/build/native-assets/albedo.ktx2"
    "${CMAKE_SOURCE_DIR}/build/native-assets/normal.ktx2"
    "${CMAKE_SOURCE_DIR}/build/native-assets/arm.ktx2")
  add_custom_command(
    OUTPUT "${CMAKE_SOURCE_DIR}/assets/materials.gen.hpp" ${EMERGENT_BAKED_PACKS}
    COMMAND "${EMERGENT_NODE}" "${CMAKE_SOURCE_DIR}/tools/assets/textures.mjs"
            --size ${EMERGENT_BAKED_SIZE} --level 6
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/assets/textures.mjs"
            "${CMAKE_SOURCE_DIR}/tools/assets/ktx2.mjs"
            "${CMAKE_SOURCE_DIR}/tools/assets/png.mjs"
            "${CMAKE_SOURCE_DIR}/assets/textures/materials.mjs"
    COMMENT "Packing the CC0 bake into build/native-assets/*.ktx2"
    VERBATIM)
  add_custom_target(emergent_baked_materials
    DEPENDS "${CMAKE_SOURCE_DIR}/assets/materials.gen.hpp" ${EMERGENT_BAKED_PACKS})
endif()

add_library(emergent_materials STATIC native/src/materials.cpp)
target_include_directories(emergent_materials PUBLIC native/include)
# The generated bake table lives beside the sources rather than in include/, so a
# clean checkout without an asset build still configures.
target_include_directories(emergent_materials PRIVATE assets)
if(TARGET emergent_baked_materials)
  add_dependencies(emergent_materials emergent_baked_materials)
endif()
set_target_properties(emergent_materials PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_materials PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_library(emergent_geometry STATIC native/src/geometry.cpp)
target_include_directories(emergent_geometry PUBLIC native/include)
target_link_libraries(emergent_geometry PUBLIC emergent_materials)
set_target_properties(emergent_geometry PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_geometry PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(emergent_material_geometry_test native/tests/material_geometry_test.cpp)
target_link_libraries(emergent_material_geometry_test PRIVATE emergent_geometry)
if(UNIX)
  target_compile_options(emergent_material_geometry_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME emergent_material_geometry COMMAND emergent_material_geometry_test)

# The atmosphere is a CPU-side single-scattering sky (Preetham-style, with a
# night model swapped in below the horizon). The renderer uploads its result
# into an environment cubemap, but the model itself is pure maths, so it is
# tested here rather than behind a GPU we may not have.
add_library(emergent_atmosphere STATIC native/src/atmosphere.cpp)
target_include_directories(emergent_atmosphere PUBLIC native/include)
set_target_properties(emergent_atmosphere PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_atmosphere PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(emergent_atmosphere_test native/tests/atmosphere_test.cpp)
target_link_libraries(emergent_atmosphere_test PRIVATE emergent_atmosphere)
if(UNIX)
  target_compile_options(emergent_atmosphere_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME emergent_atmosphere COMMAND emergent_atmosphere_test)

# The KTX2 container reader. The renderer binds three sampler2DArray maps and
# this is what fills them with the CC0 bake instead of neutral grey. It links
# the libzstd the project already vendors rather than pulling in KTX-Software,
# because reading a four-field header does not need a Basis transcoder.
add_library(emergent_ktx2 STATIC native/src/ktx2.cpp)
target_include_directories(emergent_ktx2 PUBLIC native/include)
target_link_libraries(emergent_ktx2 PUBLIC libzstd_static)
set_target_properties(emergent_ktx2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_ktx2 PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# The reader, tested against a real KTX2 file committed to the tree and written
# by the same writer the asset packer uses. The fixture is generated by
# tools/assets/fixture.mjs; regenerate it rather than editing it.
add_executable(emergent_ktx2_test native/tests/ktx2_test.cpp)
target_link_libraries(emergent_ktx2_test PRIVATE emergent_ktx2)
# Absolute paths, because ctest runs a test from the build directory and a
# relative fixture path there fails to open. A test that cannot find its fixture
# and reports success is worse than no test.
target_compile_definitions(emergent_ktx2_test PRIVATE
  EMERGENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  EMERGENT_NATIVE_ASSET_DIR="${CMAKE_SOURCE_DIR}/build/native-assets")
if(UNIX)
  target_compile_options(emergent_ktx2_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
# The real-pack checks read build/native-assets, and they skip -- loudly, but
# skip -- when it is absent. Without this dependency a build of this target alone
# would run them against nothing and report success, which is the one outcome
# worse than a failure.
if(TARGET emergent_baked_materials)
  add_dependencies(emergent_ktx2_test emergent_baked_materials)
endif()
add_test(NAME emergent_ktx2 COMMAND emergent_ktx2_test)

# The world, assembled into the bytes the shader reads. This is the missing link
# that let the renderer fall back to instanced unit cubes: nothing connected the
# world generator to a vertex buffer, so the cube path worked and hid the fact
# that it was not the game. It is plain CPU code with no Vulkan in it, so the
# buffer invariants can be walked in CI on a machine with no graphics device.
add_library(emergent_scene STATIC native/src/scene_mesh.cpp)
target_include_directories(emergent_scene PUBLIC native/include)
target_link_libraries(emergent_scene PUBLIC emergent_geometry emergent_world)
set_target_properties(emergent_scene PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_compile_options(emergent_scene PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(emergent_scene_mesh_test native/tests/scene_mesh_test.cpp)
target_link_libraries(emergent_scene_mesh_test PRIVATE emergent_scene)
if(UNIX)
  target_compile_options(emergent_scene_mesh_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME emergent_scene_mesh COMMAND emergent_scene_mesh_test)

# ---------------------------------------------------------------------------
# Shader compilation
# ---------------------------------------------------------------------------
# The GLSL in native/shaders is compiled to SPIR-V at build time when glslc is
# available (it ships with the Vulkan SDK and with shaderc). It is optional on
# purpose: the engine, the tests and CI must all build and run on a machine
# with no shader toolchain, and the renderer already reports a missing module
# by name rather than failing to start. A build with glslc gets a renderer
# that can actually draw.
find_program(EMERGENT_GLSLC NAMES glslc glslangValidator)

set(EMERGENT_SHADER_OUTPUTS)
foreach(stage vert frag)
  if(EMERGENT_GLSLC)
    set(_in ${CMAKE_CURRENT_SOURCE_DIR}/native/shaders/scene.${stage})
    set(_out ${CMAKE_CURRENT_SOURCE_DIR}/native/shaders/scene.${stage}.spv)
    if(EMERGENT_GLSLC MATCHES "glslangValidator$")
      set(_cmd ${EMERGENT_GLSLC} -V ${_in} -o ${_out})
    else()
      set(_cmd ${EMERGENT_GLSLC} -fshader-stage=${stage} ${_in} -o ${_out})
    endif()
    add_custom_command(OUTPUT ${_out}
      COMMAND ${_cmd}
      DEPENDS ${_in}
      COMMENT "Compiling scene.${stage} to SPIR-V"
      VERBATIM)
    list(APPEND EMERGENT_SHADER_OUTPUTS ${_out})
  endif()
endforeach()

if(EMERGENT_GLSLC)
  add_custom_target(emergent_shaders ALL DEPENDS ${EMERGENT_SHADER_OUTPUTS})
  message(STATUS "EMERGENT: shader compiler found (${EMERGENT_GLSLC}); native/shaders will be compiled to SPIR-V.")
else()
  message(STATUS "EMERGENT: no glslc/glslangValidator found. The engine, tests and CI build and run without it; VulkanRenderBackend will report 'shader module unavailable' instead of drawing.")
endif()

# ---------------------------------------------------------------------------
# Full native engine target
# ---------------------------------------------------------------------------
# The engine executable still needs seven more upstream libraries (meshoptimizer,
# Recast/Detour, Volk, VMA, Flecs, miniaudio, Ozz) and a Vulkan loader. None of
# those are vendored, so this target is gated: the default is ON because a
# half-built engine is worse than none, but -DEMERGENT_BUILD_ENGINE=OFF leaves a
# configureable, buildable and testable project containing the parts that are
# real (physics). That distinction is the point: it is now impossible to
# mistake this tree for something that has been verified end to end.
option(EMERGENT_BUILD_ENGINE "Build the full Vulkan engine executable" ON)

if(NOT EMERGENT_BUILD_ENGINE)
  message(STATUS "EMERGENT: engine target disabled (-DEMERGENT_BUILD_ENGINE=OFF); building the verified physics, animation, frame loop and renderer-interface subsystems only.")
  return()
endif()

find_package(Vulkan REQUIRED)

function(require_target label)
  if(NOT TARGET ${ARGN})
    if(EMERGENT_REQUIRE_UPSTREAM)
      message(FATAL_ERROR "EMERGENT requires upstream ${label}. Supply its pinned source tree, configure -DEMERGENT_FETCH_DEPS=ON, or use -DEMERGENT_BUILD_ENGINE=OFF to build only the verified physics subsystem.")
    else()
      message(WARNING "Upstream ${label} is unavailable; native target will not be production-complete.")
    endif()
  endif()
endfunction()

require_target("Jolt Physics" Jolt)
require_target("meshoptimizer" meshoptimizer)
require_target("Recast/Detour" RecastNavigation::Detour)
require_target("Volk" volk)
require_target("Vulkan Memory Allocator" GPUOpen::VulkanMemoryAllocator)
require_target("Flecs" flecs::flecs_static)
require_target("miniaudio" miniaudio)
require_target("Ozz animation base" ozz_base)
require_target("Ozz animation runtime" ozz_animation)
require_target("Ozz animation offline builders" ozz_animation_offline)
require_target("Zstandard static library" libzstd_static)

add_executable(emergent_native
  native/src/main.cpp
  native/src/engine.cpp
  native/src/vulkan_backend.cpp
  native/src/vulkan_render.cpp
  native/src/optimization.cpp
  native/src/navigation.cpp
  native/src/ecs_world.cpp
  native/src/audio.cpp
  native/src/vma_impl.cpp)
target_include_directories(emergent_native PRIVATE native/include)
target_link_libraries(emergent_native PRIVATE emergent_frame_loop emergent_render emergent_physics emergent_animation emergent_assets emergent_profiling emergent_scene emergent_atmosphere emergent_ktx2 meshoptimizer RecastNavigation::Detour RecastNavigation::DetourCrowd volk GPUOpen::VulkanMemoryAllocator flecs::flecs_static miniaudio ozz_base ozz_animation ozz_animation_offline libzstd_static)
if(EMERGENT_GLSLC)
  add_dependencies(emergent_native emergent_shaders)
endif()
target_compile_definitions(emergent_native PRIVATE EMERGENT_UPSTREAM_ONLY=1)

if(UNIX)
  target_link_libraries(emergent_native PRIVATE dl)
  target_compile_options(emergent_native PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
endif()

add_executable(emergent_benchmark native/tools/benchmark.cpp native/src/optimization.cpp)
target_include_directories(emergent_benchmark PRIVATE native/include)
target_link_libraries(emergent_benchmark PRIVATE meshoptimizer emergent_physics)
if(UNIX)
  target_compile_options(emergent_benchmark PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_test(NAME emergent_native_self_test COMMAND emergent_native)
add_test(NAME emergent_native_benchmark_smoke COMMAND emergent_benchmark)
# The frame loop as the shipped binary runs it, not just as a test drives it.
# A loop that only works under a test harness is not a loop.
add_test(NAME emergent_native_frame_loop COMMAND emergent_native --frames 300 --walk)
add_test(NAME emergent_native_frame_loop_realtime COMMAND emergent_native --realtime 0.25 --walk)

# The world, streamed all the way to the renderer, on a machine with no Vulkan
# driver. The renderer used to draw instanced cubes because nothing connected the
# world generator to a vertex buffer; this asserts that the connection is made
# and that it produces a city, which is the part that can be checked without a
# GPU. The pixels are not checked here and cannot be — see
# docs/ENGINE_VERIFICATION.md.
# Enough frames for the amortized build to finish. It needs 14 at the default
# budget for this slice, and a test that runs 5 and asserts a finished mesh
# would be asserting nothing at all.
add_test(NAME emergent_native_scene_stream COMMAND emergent_native --scene 7 --frames 240)
# The amortization itself. The point of the builder is that no single frame pays
# for the whole slice, and a screenshot cannot show that -- a profile can, and so
# can the number the binary prints. Asserting the mesh is still the full one
# alongside it is the other half: a spread-out build that quietly drops geometry
# would satisfy the first check alone.
add_test(NAME emergent_native_scene_stream_is_amortized
         COMMAND emergent_native --scene 7 --frames 240)
set_tests_properties(emergent_native_scene_stream_is_amortized PROPERTIES
  PASS_REGULAR_EXPRESSION
    "Scene mesh: [1-9][0-9]+ vertices.*Scene build: ([2-9]|[1-9][0-9]+) frames")
# The binary has to say where it is looking for the packed KTX2 maps. It used to
# find them only by a path relative to the working directory, and find nothing,
# and say nothing about it: a city with no materials looks like a material bug
# rather than a missing file. Two tests, because the default and the override
# fail in opposite directions.
add_test(NAME emergent_native_asset_dir_default
         COMMAND emergent_native --scene 7 --frames 1)
set_tests_properties(emergent_native_asset_dir_default PROPERTIES
  PASS_REGULAR_EXPRESSION "Material maps: .*native-assets")
add_test(NAME emergent_native_asset_dir_override
         COMMAND emergent_native --scene 7 --frames 1 --assets somewhere/else)
set_tests_properties(emergent_native_asset_dir_override PROPERTIES
  PASS_REGULAR_EXPRESSION "Material maps: somewhere/else")
# Two conditions, because either alone is too weak. Requiring only a non-zero
# vertex count passes even if every building fails to emit, since the ground,
# the roads and the trees are still there — checked by deleting the building
# mass and watching this test still pass. The buildings line is the one that
# actually asserts a city was assembled.
set_tests_properties(emergent_native_scene_stream PROPERTIES
  PASS_REGULAR_EXPRESSION "Scene slice: [1-9][0-9]* buildings.*Scene mesh: [1-9][0-9]+ vertices")
