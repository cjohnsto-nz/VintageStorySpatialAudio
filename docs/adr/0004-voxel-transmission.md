# ADR 0004: Thickness-aware transmission

**Status:** Accepted, 19 Sep 2026

## Context

Steam Audio's transmission multiplies per-*surface* coefficients along the direct path, up to `numTransmissionRays` surfaces. With boundary-only meshing (ADR 0003), one stone block and six stone blocks both produce two surfaces, so they would sound the same. We want glass, a wooden door, one stone block and a mountain to sound different.

## Decision

- The direct simulation thread computes transmission itself.
- It runs a voxel DDA from listener to source over a compact, per-chunk material grid, accumulating the actual path length through each material: `T_b = Π exp(−α_b(material) · length)` per band.
- The result goes into `IPLDirectEffectParams.transmission[3]` with `IPL_TRANSMISSIONTYPE_FREQDEPENDENT`.
- Occlusion still comes from Steam Audio (volumetric).

## Consequences

- We maintain a material grid alongside the mesh, built by the same snapshot pipeline.
- Attenuation coefficients per material are tunable data (`acousticmaterials.json`), checked against a thickness test matrix in Phase 5.
