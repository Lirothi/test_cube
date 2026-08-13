# Crest reference sources (MIT)

Reference-only copies from https://github.com/wave-harmonic/crest (master, 2026-08-14) for
docs/ocean_surf_sim_plan.md. NOT part of the build. License: see LICENSE in this folder.

- `UpdateDynWaves.compute` — THE wave-equation kernel (five-point Laplacian, height+velocity
  RG channels, damping, depth-based attenuation). The numerics our S1 copies.
- `UpdateFoam.compute` — foam sim kernel: semi-Lagrangian advection, exponential fade,
  Jacobian whitecap deposit, depth-driven shoreline deposit (the part we REPLACE with
  event-driven breaking — depth-only terms are banned by the plan's invariant 1).
- `OceanFoam.hlsl` — how the surface shader consumes the sim texture (two-threshold layering).
- `LodDataMgrDynWaves.cs` — dyn-waves manager: simulation frequency, gravity multiplier,
  stability/courant handling.
- `LodDataMgrFoam.cs` — foam manager: substep data, texture format (R16F).
- `LodDataMgrPersistent.cs` — the shared persistent-sim cadence: fixed-dt substeps with
  catch-up, ping-pong, prewarm on teleport.
- `SimSettingsWave.cs` / `SimSettingsFoam.cs` — every knob with defaults and ranges.
