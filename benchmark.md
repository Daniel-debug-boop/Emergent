# EMERGENT reproducible runtime benchmark

## Standard vs adaptive

Use the same device, browser, viewport, seed and quality setting.

Standard:

```text
http://<your-host>:<port>/index.html?benchmark=standard&seed=173927&quality=HIGH
```

Adaptive:

```text
http://<your-host>:<port>/index.html?benchmark=adaptive&seed=173927&quality=HIGH
```

The benchmark runs for approximately eight seconds and stores the result in `document.body.dataset.benchmark` as JSON.

## Reported metrics

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

The render wall time is CPU-side timing around WebGL command submission. It is **not** a claimed GPU execution time.

For an actual GPU metric, add a verified `EXT_disjoint_timer_query_webgl2` measurement path and validate it on the target GPU. Do not substitute CPU timing for GPU timing.

## CPU/world-generation benchmark

`node test_project.mjs` generates several deterministic worlds and reports measured generation time. The current validation run measured:

| Seed | Generation time | Districts | Roads | Buildings | Trees | Businesses | NPCs | Cars |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 173927 | 316.501 ms | 50 | 672 | 3,077 | 1,838 | 714 | 520 | 150 |
| 173928 | 198.898 ms | 35 | 674 | 2,159 | 2,019 | 568 | 520 | 150 |
| 424242 | 238.006 ms | 52 | 708 | 3,365 | 1,798 | 885 | 520 | 150 |

These are container CPU measurements for generation only, not on-device gameplay FPS.

## Environment limitation during this build

The available Chromium environment blocked local-page navigation with an administrator policy and also could not initialize a usable WebGL2 GPU context in the headless session. Because of that, no browser FPS/GPU result is claimed as verified here.
