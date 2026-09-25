# EMERGENT reproducible runtime benchmark

Two benchmark paths exist, and they measure different things. Both report CPU-side
wall time only; neither fabricates a GPU number.

## 1. Headless benchmark (runs anywhere, in CI)

The game is executed for real inside Node against a validating WebGL2 surface
(`tools/headless_runtime.mjs`). No browser, no GPU, no display required.

```bash
npm run bench            # STANDARD renderer, 300 frames (~10s of simulated time)
npm run bench:adaptive   # ADAPTIVE renderer, same seed and quality
```

Both print a JSON object containing the game's own `document.body.dataset.benchmark`
result plus the harness's WebGL statistics (draw calls, drawn vertices, buffer
uploads, non-finite uploads, shader compiles, link status, validation errors).
The command exits non-zero if the benchmark is missing, geometry is implausibly
small, no streaming happened, or any WebGL validation error was recorded.

Other flags: `--frames=N`, `--seed=N`, `--quality=LOW|MEDIUM|HIGH|ULTRA`.

### Measured on the build container (Node 22, seed 173927, quality HIGH)

| Metric | STANDARD | ADAPTIVE |
|---|---:|---:|
| Wall time for 300 frames | 1736.6 ms | 973.7 ms |
| CPU scene render time / frame | 0.102 ms | 0.076 ms |
| Dynamic buffer builds | 243 | 58 |
| Average dynamic build time | 3.357 ms | 5.550 ms |
| Recomputed objects / frame | 44.24 | 9.85 |
| Reused objects / frame | 0.40 | 34.69 |
| Static vertices | 41,028 | 22,038 |
| Dynamic vertices | 15,345 | 15,345 |

The adaptive renderer performs the same work with far fewer recomputations
(9.85 vs 44.24 per frame) and roughly halves the wall time of the run. Note the
per-rebuild cost is higher in adaptive mode: it does fewer, less frequent,
full-quality rebuilds. The player is stationary in this scenario, so it measures
the idle-path worst case for the adaptive schedule, not movement.

These are container CPU measurements. They are not on-device gameplay FPS.

## 2. In-browser benchmark (device-specific)

Use the same device, browser, viewport, seed and quality setting.

```text
http://<your-host>:<port>/index.html?benchmark=standard&seed=173927&quality=HIGH
http://<your-host>:<port>/index.html?benchmark=adaptive&seed=173927&quality=HIGH
```

The benchmark runs for approximately eight seconds and stores the result in
`document.body.dataset.benchmark` as JSON.

### Reported metrics

- FPS
- average scene render wall time
- average dynamic buffer build wall time
- dynamic buffer build count
- average recomputed objects
- average reused objects
- average visible objects
- static/dynamic vertex counts
- streaming operations
- streamed geometry size
- world entity counts

The render wall time is CPU-side timing around WebGL command submission. It is
**not** a claimed GPU execution time.

For an actual GPU metric, add a verified `EXT_disjoint_timer_query_webgl2`
measurement path and validate it on the target GPU. Do not substitute CPU timing
for GPU timing.

## CPU/world-generation benchmark

`npm run test:project` generates several deterministic worlds and reports measured
generation time. The current validation run measured:

| Seed | Generation time | Districts | Roads | Buildings | Trees | Businesses | NPCs | Cars |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 173927 | 448.4 ms | 50 | 672 | 3,077 | 1,838 | 714 | 520 | 150 |
| 173928 | 317.8 ms | 35 | 674 | 2,159 | 2,019 | 568 | 520 | 150 |
| 424242 | 257.8 ms | 52 | 708 | 3,365 | 1,798 | 885 | 520 | 150 |

These are container CPU measurements for generation only, not on-device gameplay FPS.

## What is still not measured

GPU execution time and VRAM usage. A real GPU measurement path must be validated on
the target device before either number is quoted, so neither appears above.
