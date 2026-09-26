# EMERGENT upstream stack

EMERGENT uses mature upstream libraries for engine infrastructure wherever they genuinely replace bespoke technology.

Current native runtime dependencies:
- Jolt Physics 5.6.0 — rigid-body physics and collision.
- meshoptimizer 1.2 — mesh optimization.
- Recast/Detour 1.6.0 — navigation and crowd simulation.
- Volk 1.4.328 — Vulkan loader.
- Vulkan Memory Allocator 3.3.0 — Vulkan memory allocation.
- Flecs 4.0.5 — ECS.
- miniaudio 0.11.25 — audio runtime.
- ozz-animation 0.17.0 — skeletal animation runtime and offline builders.
- Zstandard 1.5.7 — compressed content packs and their integrity checks.
- Tracy 0.13.0 — live profiling capture, opt-in via `EMERGENT_ENABLE_TRACY=ON`.

Web runtime dependencies:
- Rapier 0.21.0 (Apache-2.0) — `@dimforge/rapier3d-compat`, the character
  controller, swept collision, autostep, snap-to-ground, slope limits and the
  terrain heightfield collider for the shipped WebGL2 game.

Rapier is a separate line from Jolt on purpose. Jolt is the authoritative
physics for the native engine; the web build is a single self-contained ES
module with the WASM inlined, and a WASM build of Jolt is not one of the things
upstream ships prebuilt. Two physics engines is a real cost, so the split is
stated plainly: `native/` uses Jolt, the browser game uses Rapier, and neither
talks to the other.

The dependency is listed here on the same evidence as the native ones — it is
used, not merely installed. `physics.mjs` is the only file that imports it, and
`test_physics.mjs` executes the real engine, so the symbols below are reached on
every CI run:

```
$ node -p "require('./node_modules/@dimforge/rapier3d-compat/package.json').version"
0.21.0
$ ls -l node_modules/@dimforge/rapier3d-compat/dist/rapier.mjs | awk '{print $5}'
4340292
$ grep -c "computeColliderMovement" dist/rapier.mjs      # shipped, not just installed
2
```

The build copies that module verbatim to `dist/vendor/rapier.mjs` and
`index.html` resolves the bare specifier through an import map, so the browser
loads byte-identical code to what the tests import from `node_modules`. The
build fails if a vendored file is not mapped, which turns the failure mode from
a 404 on the first frame into a build error.

There is no bundler. The reason is in `tools/build.mjs`: the shipped artefact is
exactly the reviewed source, with no transform that could diverge from what the
tests execute.

These dependencies are fetched or supplied as source trees. With EMERGENT_REQUIRE_UPSTREAM=ON, missing required targets are configuration errors rather than permission to compile a home-grown substitute.

A dependency is only listed here once something in the tree calls it. Because a static archive nothing references is dropped by the linker without a warning, that is checked directly:

```
$ nm -C build/native/emergent_native | grep -c "ozz::"   # -> 188
$ nm -C build/native/emergent_native | grep -c "ZSTD_"   # -> 450
$ nm -C build/native/emergent_native | grep -c "JPH::"   # -> 4657
```

Tracy's count is 0 in the default build because `EMERGENT_ENABLE_TRACY` is off there; the Tracy-enabled configuration is the one that carries the symbols, and CI builds and tests both.

No windowing library is on this list. `SurfaceProvider` is an interface, not a dependency, and the reasoning is in `docs/OPEN_SOURCE_DECISIONS.md`. A window library cannot be stubbed without inventing a fake display server, so EMERGENT takes the four things a renderer actually asks of a window and lets a platform backend supply them.

The Forge remains an upstream renderer/framework candidate, but it is not falsely claimed as integrated until its actual source/build system is present and its renderer replaces the current Vulkan bootstrap rather than merely coexisting with it.

KTX-Software/Basis Universal and Slang are deliberately not integrated. A compressed-texture path needs authored `.ktx2` assets to decode and this tree has none, and a shading-language compiler has no offline compilation step to replace while the compute shaders stay driver-compiled GLSL. Neither is listed as a dependency because neither is used.
