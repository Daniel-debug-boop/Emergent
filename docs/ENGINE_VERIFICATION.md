# Engine verification — 2026-09-24

## Passing in the supplied environment

- `npm test`
- `npm run check`
- CMake Release configure/build with upstream fetching disabled
- CTest: 2/2 tests passed
- Native self-test
- Native benchmark smoke

## Important environment limitation

The supplied environment cannot resolve GitHub, so `EMERGENT_FETCH_DEPS=ON` could not download Jolt, meshoptimizer, Recast/Detour, Volk or VMA. The build was therefore verified in capability-probe mode rather than falsely presenting those libraries as linked.

The local environment also does not expose a usable Vulkan device/ICD. The native executable correctly reports Vulkan initialization failure instead of fabricating GPU execution.

On a production build machine, supply the pinned upstream source trees or install their CMake packages and configure the corresponding `EMERGENT_*_DIR` variables. The CMake graph will then link the actual upstream targets.
