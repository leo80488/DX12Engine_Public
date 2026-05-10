# Status & Limitations

- Single-window, single-viewport.
- No automated tests.
- No DXR hardware-RT for primary visibility — DXR is used only by the DDGI inline-RayQuery trace.
- Hot-reload coverage today: 9 of ~30 passes are wired (GBuffer / Lighting / Shadow eager,
  Skybox / Transparent / Outline / SpotShadow / VolumetricFog / Picking lazy). Compute-only passes
  with hand-built PSOs need an explicit override to opt in.
- Animation: linear blend skinning only (no dual-quaternion / no compute spline path).
- Networking: not implemented.
