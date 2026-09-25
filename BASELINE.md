# EMERGENT 3D Migration Baseline

## Existing project inspected
The supplied EMERGENT-evolved project was a Canvas 2D prototype. Its implementation contained deterministic 9600x9600 world generation, 520 NPCs, 150 vehicles, businesses/economy/weather, district discovery, chunk caching and four 2D renderer modes.

## Migration
The Canvas renderer has been replaced by a self-contained WebGL2 pipeline. The simulation remains separate from rendering and was adapted into 3D coordinates.

## Objective runtime measurements
The browser in the build container could not be used for a reliable timed WebGL2 session without hanging/terminating, so this file intentionally contains **no fabricated FPS or GPU results**.

The game HUD reports on-device:
- FPS
- CPU render wall time
- static vertex count
- dynamic vertex count
- recomputation count
- reuse count
- streaming operations
- NPC/vehicle/building population

GPU timing is not reported because a WebGL timer-query extension must be verified on the actual target GPU before that number can be trusted.

## Research status
Milestone reached: genuine 3D playable foundation + deterministic procedural world + spatial streaming + adaptive 3D recomputation experiment.

Not yet claimed complete: temporal reprojection, advanced GPU-driven culling, photorealistic assets, full navigation/pathfinding, sophisticated vehicle collision, and AAA-level animation.
