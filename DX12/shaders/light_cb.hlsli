#ifndef LIGHT_CB_HLSLI
#define LIGHT_CB_HLSLI

// light_cb.hlsli — single source of truth for the LightCB layout.
//
// The C++ side declares the matching POD struct in Renderer.cpp
// (`namespace { struct LightCB { ... }; }` in the same file). When fields are
// added or reordered, edit BOTH this header and that struct in lockstep — the
// HLSL packing rules (each member must fit in a 16-byte row, float3 cannot
// straddle, arrays pad each element to 16) are mirrored on the C++ side via
// scalar-stride layout, so the byte offsets stay aligned without DirectXMath.
//
// Different passes bind LightCB at different cbuffer registers — the deferred
// LightingPass at b1, the forward TransparentPass at b2 (its VS owns b1 for
// PerViewCB) — so the consumer must `#define LIGHT_CB_REGISTER` to the
// desired slot literal before including this header. Macros expand inside
// `register(...)`, so the cbuffer declaration ends up bound at the right slot
// without any per-pass duplication of the layout.
//
// Usage (deferred lighting pass):
//
//     #define LIGHT_CB_REGISTER b1
//     #include "light_cb.hlsli"
//
// Usage (forward transparent pass):
//
//     #define LIGHT_CB_REGISTER b2
//     #include "light_cb.hlsli"
//
// All fields are at global scope (a flat `cbuffer`, not a `ConstantBuffer<T>`)
// so existing access sites that read bare `lightDir`, `cameraPos`, etc. keep
// compiling unchanged.

#ifndef LIGHT_CB_REGISTER
#error "Define LIGHT_CB_REGISTER (e.g. b1) before #include \"light_cb.hlsli\""
#endif

cbuffer LightCB : register(LIGHT_CB_REGISTER, space0)
{
    // ---- Direct directional light + camera + IBL params --------------------
    float3   lightDir;          float _p0;
    float3   lightColor;        float _p1;
    float3   cameraPos;         float _p3;
    float4x4 invViewProj;
    uint     iblRadianceMips;
    float    iblStrength;
    uint     iblUseSH;          // 0 = sample gIrradiance cubemap, 1 = evaluate gSkySH
    float    aerialMaxDistKm;   // Aerial Perspective LUT far slice (0 = disabled)

    // ---- CSM shadow data — 4 cascades --------------------------------------
    // Cascades 0..2 are the standard near cascades. Cascade 3 is a dedicated
    // ultra-far cascade for terrain self-shadow (mountain silhouettes onto
    // plain at sunrise/sunset). Layout matches Renderer.cpp's LightCB
    // mirror exactly — keep field order in lockstep.
    float4x4 shadowMatrix[4];          // per-cascade light VP (row-major, transposed)
    float4   cascadeSplits;            // view-Z far boundary of each cascade
    float4   cascadeTexelWorldSize;    // world m/texel per cascade (receiver bias)
    float    shadowBias;               // legacy uniform receiver bias
    float    shadowStrength;           // 0 = disabled, 1 = full shadows
    float    shadowMapTexelSize;       // 1.0 / kShadowMapSize, precomputed
    float    shadowBlendRange;         // cascade blend zone width in view-Z (world units)
    uint     shadowFrameIndex;         // per-frame PCF dither rotation key (TAA smoothing)
    float    shadowNormalOffset;       // receiver normal-offset strength (texels). 0 disables.
    float    _shadowPad0;              // explicit pad — see C++ mirror in Renderer.cpp
    float    _shadowPad1;
    float3   cameraForward;            // unit camera forward (view-Z cascade selection)
    float    _shadowPad2;

    // ---- Clustered lighting params -----------------------------------------
    float4x4 viewMatrix;        // world → view, used to pick clusters by view-Z
    float    clusterNearZ;
    float    clusterFarZ;
    uint     clusterLightCount;
    float    nprMinBrightness;  // NPR minimum brightness floor (0 = unused for PBR)

    // ---- Reflection-probe pool ---------------------------------------------
    // Number of valid entries in gReflectionProbes — 0 → skip probe loop
    // entirely, fall back to gRadiance only.
    uint     reflectionProbeCount;
    float3   _reflectionPad;

    // ---- DDGI integration --------------------------------------------------
    // Number of bound DDGI volumes (0..DDGI_MAX_VOLUMES). 0 → skip the entire
    // DDGI sampling block in Lighting.ps and use Sky IBL diffuse only.
    uint     ddgiVolumeCount;
    // Master DDGI on/off + global scales — mirrors IndirectLightingSettings.
    uint     ddgiEnabled;          // 0 / 1
    float    ddgiDiffuseScale;
    float    skyIBLDiffuseScale;

    // Near-field AO strength on the DDGI diffuse path. DDGI already integrates
    // probe-distance visibility (mid-scale occlusion), so re-applying full
    // screen-space AO double-darkens. This knob attenuates AO on the DDGI
    // portion only; the Sky-IBL fallback portion still receives full AO.
    //   0.0 = no AO on DDGI (trust probes)
    //   1.0 = full AO on DDGI (legacy behaviour, double-occludes)
    //   ~0.4 = recommended balance (keeps near-field contact darkening)
    float    ddgiAONearFieldStrength;
    // Global view-mode switch (Lit/Unlit/Wireframe). Repurposed from a DDGI
    // pad — same 4-byte slot, no layout change. See view_mode_common.hlsli for
    // the VIEW_MODE_* enum and Renderer::ViewMode for the C++ mirror.
    uint     viewMode;
    float    _ddgiPad1;
    float    _ddgiPad2;
};

#endif // LIGHT_CB_HLSLI
