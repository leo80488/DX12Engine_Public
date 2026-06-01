#ifndef VIEW_MODE_COMMON_HLSLI
#define VIEW_MODE_COMMON_HLSLI

// view_mode_common.hlsli — shared constants for the global Lit/Unlit/Wireframe
// view-mode switch (Unity/Unreal-style "viewmode" dropdown).
//
// The active mode is carried in LightCB.viewMode (see light_cb.hlsli) so any
// shader that includes light_cb.hlsli can branch on it. Mirror enum on the
// C++ side: Renderer::ViewMode (include/Graphics/Renderer.h).
//
//   Lit       — normal shading (default; no override).
//   Unlit     — output base color only, bypass the BRDF.
//   Wireframe — geometry PSOs rasterize edges only (FILL_MODE_WIREFRAME) and
//               the shaded edge pixels are forced to a single flat colour so
//               the wireframe reads as uniform lines on a dark background.

#define VIEW_MODE_LIT       0u
#define VIEW_MODE_UNLIT     1u
#define VIEW_MODE_WIREFRAME 2u

// Flat colour emitted for every wireframe edge pixel. Linear-space, fed through
// the same tonemap as the rest of the scene — picked bright enough to stay
// readable after exposure/tonemapping.
#define WIREFRAME_COLOR float3(0.10, 0.90, 0.75)

#endif // VIEW_MODE_COMMON_HLSLI
