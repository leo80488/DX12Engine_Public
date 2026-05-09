// ProbeCaptureSky.ps.hlsl — strips Skybox.ps down to "sample one cubemap with
// the world-space direction the VS produced". Used by the reflection probe
// capture pass to fill any pixel the opaque pass left untouched (i.e. the sky
// the probe sees through windows / doorways) so the prefiltered cube doesn't
// have a black hole where geometry didn't cover.
//
// Pairs with Skybox.vs.hlsl (which also outputs `dir` from the cube vertex
// position). The Skybox.ps's sun disk / moon / starfield logic is intentionally
// dropped here — those are large-scale time-of-day artefacts that should be
// re-rendered every frame, not baked into static probe data.

TextureCube  gSkyCube : register(t6, space0);
SamplerState gSampler : register(s0);

struct PSIn
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;  // local cube position = world direction (no translation)
};

float4 main(PSIn i) : SV_TARGET
{
    return float4(gSkyCube.SampleLevel(gSampler, normalize(i.dir), 0).rgb, 1.0);
}
