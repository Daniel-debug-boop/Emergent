# CHANGELOG

## Completion pass — 3D game integration

- Preserved the existing deterministic world/simulation data model.
- Split deterministic world generation into `world.mjs` for testability.
- Expanded district/building/vegetation/business generation while preserving seed reproducibility.
- Added coherent gameplay loop with world-state-driven delivery missions.
- Added mission pickup/delivery stages, rewards, progression rank and business consequences.
- Added persistent player/business/NPC/mission/event save state.
- Added spatially indexed building collision checks.
- Added NPC daily scheduling, needs, mood and activity memory.
- Added rain-aware NPC shelter behavior.
- Added lane-aware traffic speed control and rain slowdown.
- Added traffic incident events from close headway in poor weather.
- Added procedural architectural details: roofs, windows, doors, signs, warehouse differences and street furniture.
- Added low-poly 3D character parts, vehicles and vegetation.
- Added animated water movement at the vertex stage.
- Added a geometric sun-shadow pass for nearby architecture.
- Added adaptive dynamic-buffer reuse with distance/importance-based per-NPC update cadence.
- Added renderer quality levels LOW/MEDIUM/HIGH/ULTRA.
- Added mobile touch movement/sprint/interact controls.
- Added player-facing HUD and developer telemetry separation.
- Added benchmark query modes for standard/adaptive comparison.
- Added deterministic and project integrity tests.

## 2026-09-24 — upstream subsystem replacement pass

- Added an actual Recast/Detour native navigation boundary with navmesh loading and crowd-agent movement APIs.
- Added explicit offline source-tree dependency inputs for Jolt, meshoptimizer, RecastNavigation, Volk and VMA.
- Added installed-package discovery for VulkanMemoryAllocator and RecastNavigation.
- Changed the Vulkan bootstrap resource path to VMA allocation/destruction instead of manual Vulkan memory selection/binding when the full upstream Vulkan build is enabled.
- Disabled Recast demos/tests/examples for dependency builds; EMERGENT consumes the libraries rather than rebuilding their sample applications.
- Added engine verification documentation and kept capability reporting truthful when upstream libraries or a Vulkan device are unavailable.
