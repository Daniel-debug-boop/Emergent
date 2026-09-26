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
  message(STATUS "EMERGENT: engine target disabled (-DEMERGENT_BUILD_ENGINE=OFF); building the verified physics subsystem only.")
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
  native/src/optimization.cpp
  native/src/navigation.cpp
  native/src/ecs_world.cpp
  native/src/audio.cpp
  native/src/vma_impl.cpp)
target_include_directories(emergent_native PRIVATE native/include)
target_link_libraries(emergent_native PRIVATE emergent_physics emergent_animation emergent_assets emergent_profiling meshoptimizer RecastNavigation::Detour RecastNavigation::DetourCrowd volk GPUOpen::VulkanMemoryAllocator flecs::flecs_static miniaudio ozz_base ozz_animation ozz_animation_offline libzstd_static)
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
