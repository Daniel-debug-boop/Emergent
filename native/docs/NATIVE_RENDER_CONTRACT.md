# Native Render Contract

The Riverside Overlook image is a visual-quality reference only. EMERGENT must generate its scene at runtime from world data.

## Required runtime representations

- terrain and mountains
- water
- roads
- buildings
- vegetation
- NPCs
- vehicles
- sky/sun/weather
- player character
- streaming/chunk state

## Required rendering progression

`world data -> scene database -> mesh/meshlet representation -> GPU visibility -> indirect submission -> shading -> temporal/adaptive passes`

The implementation must not replace these stages with a pre-rendered image or a camera-specific impostor that only works for the reference view.

## Acceptance evidence

For every optimization, record:

- feature actually enabled
- hardware/driver capability
- CPU time
- GPU time when GPU timestamps are available
- submitted draw/dispatch count
- visible object/meshlet count
- resident memory
- image quality comparison
- fallback path

No FPS, GPU time, mesh-shader support, temporal reuse, or GPU-driven claim is valid without corresponding runtime evidence.
