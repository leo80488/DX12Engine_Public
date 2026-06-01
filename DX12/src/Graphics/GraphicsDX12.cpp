#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/VideoDecoderDX12.h"
#include "Graphics/DxcCompiler.h"
#include "System/Log.h"
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <queue>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "d3dx12.h"
#include <d3d12sdklayers.h>
using Microsoft::WRL::ComPtr;

// The GPUProfiler readback ring is read with kFrameCount-1 frames of latency
// to match the swap-chain pipelining. If its ring depth ever diverges from the
// swap-chain FrameCount, BeginFrame would read a slot whose resolve is still
// in flight (a data race). The header documents this as a hard requirement;
// enforce it at compile time.
static_assert(GPUProfiler::kFrameCount == GraphicsDX12::FrameCount,
              "GPUProfiler::kFrameCount must match GraphicsDX12::FrameCount");

void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line)
{
    if (FAILED(hr))
    {
        Logger::Get().LogHResult(expr, hr, file, line);
        throw std::runtime_error("DX12 call failed");
    }
}


// ===========================================================================
// Default root signature builder
// ===========================================================================

ComPtr<ID3D12RootSignature> GraphicsDX12::CreateDefaultRootSignature()
{
    // Full slot map lives in the GraphicsDX12.h class-banner comment.
    constexpr UINT kRampTexSlot      = 26; // t16 space0 — NPR ramp texture
    constexpr UINT kBindlessTexSlot  = 27; // t0 space2 — bindless texture array
    constexpr UINT kMaterialBufSlot  = 28; // t12 space0 — MaterialBuffer (for NPR per-material params)
    constexpr UINT kEmissiveSRVSlot  = 29; // t17 space0 — GBuffer emissive SRV (deferred lighting)
    constexpr UINT kSSAOSRVSlot      = 30; // t18 space0 — SSAO texture (XeGTAO, one-frame latency)
    constexpr UINT kSkySHSRVSlot      = 31; // t19 space0 — Sky SH coefficient buffer (9 × float4)
    constexpr UINT kAerialPerspSlot   = 32; // t20 space0 — Aerial Perspective 3D LUT (Texture3D<float4>)
    constexpr UINT kSpotShadowAtlasSlot = 33; // t21 space0 — Texture2DArray<float> SpotShadowPass atlas
    constexpr UINT kSpotShadowVPSlot  = 34; // t22 space0 — StructuredBuffer<float4x4> per-slice VPs
    constexpr UINT kCustomMatCBVSlot  = 35; // b8 space0 — Phase E per-material CBV packed from shader reflection
    constexpr UINT kCustomMatTexSlot  = 36; // t0-t(N-1) space3 — Phase F per-material named texture table
    // Source of truth is GraphicsDX12::kCustomMatTextureSlots (header). Use
    // that here so root-sig + descriptor-table sizes can never drift.
    constexpr UINT kCustomMatTexCount = GraphicsDX12::kCustomMatTextureSlots;
    constexpr UINT kReflectionProbeArraySlot  = 37; // t23 space0 — TextureCubeArray<float4> reflection probe pool
    constexpr UINT kReflectionProbeBufferSlot = 38; // t24 space0 — StructuredBuffer<GPUReflectionProbe>
    constexpr UINT kReflectionProbeGridSlot   = 39; // t25 space0 — StructuredBuffer<ProbeGridEntry>
    constexpr UINT kReflectionProbeIndexSlot  = 40; // t26 space0 — StructuredBuffer<uint> probe index list
    constexpr UINT kSSRResultSlot             = 41; // t27 space0 — Texture2D<float4> SSR trace (prev-frame)
    // DDGI multi-volume: each per-volume table holds DDGI_MAX_VOLUMES SRVs.
    // Lighting.ps gates the read via `vi < ddgiVolumeCount`; unused entries
    // still hold null SRVs for validation. Must match Lighting.ps.hlsl + DDGICommon.hlsli.
    constexpr UINT kDDGIMaxVolumes      = 4; // mirror of DDGI::kMaxVolumes / DDGI_MAX_VOLUMES
    constexpr UINT kDDGIVolumeBufSlot   = 42; // t28 space0
    constexpr UINT kDDGIProbeSHSlot     = 43; // t29..t32 space0 (NumDescriptors=4)
    constexpr UINT kDDGIDepthSlot       = 44; // t33..t36 space0 (NumDescriptors=4)
    constexpr UINT kDDGIProbeDataSlot   = 45; // t37..t40 space0 (NumDescriptors=4)
    constexpr UINT totalParams = 1 + kCBVSlotCount + 2 + kSRVSlotCount + 1 + kSamplerSlotCount + kIBLSRVSlotCount + 1 + kClusterSRVSlotCount + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + 1 + 2 + 2 + 1 + 4; // = 46
    D3D12_ROOT_PARAMETER params[totalParams] = {};

    // [0] Root constants — 4 uint32s at b0 space0
    //     [0] meshDescIdx  [1] instanceOffset  [2] materialIndex  [3] outlinePixelsBits (asfloat)
    params[kRootConstantsSlot].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstantsSlot].Constants.ShaderRegister = 0;
    params[kRootConstantsSlot].Constants.RegisterSpace  = 0;
    params[kRootConstantsSlot].Constants.Num32BitValues = 4;
    params[kRootConstantsSlot].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    // [1..7] Root CBVs — b1-b7 space0
    for (UINT i = 0; i < kCBVSlotCount; ++i)
    {
        params[kCBVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[kCBVSlotBase + i].Descriptor.ShaderRegister = i + 1; // b1..b7
        params[kCBVSlotBase + i].Descriptor.RegisterSpace  = 0;
        params[kCBVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [8] Root SRV — InstanceBuffer at t0 space0
    params[kInstanceBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kInstanceBufSlot].Descriptor.ShaderRegister = 0; // t0
    params[kInstanceBufSlot].Descriptor.RegisterSpace  = 0;
    params[kInstanceBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [9] Root SRV — MeshDescriptors at t1 space0
    params[kMeshDescSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kMeshDescSlot].Descriptor.ShaderRegister = 1; // t1
    params[kMeshDescSlot].Descriptor.RegisterSpace  = 0;
    params[kMeshDescSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [10..13] Descriptor tables — 1 SRV each at t2-t5 space0
    static D3D12_DESCRIPTOR_RANGE srvRanges[kSRVSlotCount] = {};
    for (UINT i = 0; i < kSRVSlotCount; ++i)
    {
        srvRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors                    = 1;
        srvRanges[i].BaseShaderRegister                = i + 2; // t2..t5
        srvRanges[i].RegisterSpace                     = 0;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &srvRanges[i];
        params[kSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [14] Descriptor table — kMaxBindlessBuffers SRVs at t0 space1 (bindless g_Buffers[])
    static D3D12_DESCRIPTOR_RANGE bindlessRange = {};
    bindlessRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors                    = kMaxBindlessBuffers;
    bindlessRange.BaseShaderRegister                = 0; // t0
    bindlessRange.RegisterSpace                     = 1; // space1
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessSlot].DescriptorTable.pDescriptorRanges   = &bindlessRange;
    params[kBindlessSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [15..18] Descriptor tables — 1 sampler each at s0-s3
    static D3D12_DESCRIPTOR_RANGE samplerRanges[kSamplerSlotCount] = {};
    for (UINT i = 0; i < kSamplerSlotCount; ++i)
    {
        samplerRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRanges[i].NumDescriptors                    = 1;
        samplerRanges[i].BaseShaderRegister                = i;
        samplerRanges[i].RegisterSpace                     = 0;
        samplerRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kSamplerSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kSamplerSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kSamplerSlotBase + i].DescriptorTable.pDescriptorRanges   = &samplerRanges[i];
        params[kSamplerSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [19..20] Descriptor tables — 1 SRV each at t6-t7 space0 (IBL cubemaps)
    static D3D12_DESCRIPTOR_RANGE iblRanges[kIBLSRVSlotCount] = {};
    for (UINT i = 0; i < kIBLSRVSlotCount; ++i)
    {
        iblRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        iblRanges[i].NumDescriptors                    = 1;
        iblRanges[i].BaseShaderRegister                = i + kSRVSlotCount + 2; // t6, t7
        iblRanges[i].RegisterSpace                     = 0;
        iblRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kIBLSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kIBLSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kIBLSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &iblRanges[i];
        params[kIBLSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [22] Descriptor table — 3 SRVs at t9-t11 space0 (CSM shadow cascade maps)
    static D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = kShadowSRVCount;
    shadowRange.BaseShaderRegister                = kSRVSlotCount + 2 + kIBLSRVSlotCount; // t9
    shadowRange.RegisterSpace                     = 0;
    shadowRange.OffsetInDescriptorsFromTableStart = 0;

    params[kShadowSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kShadowSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kShadowSRVSlot].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[kShadowSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [23-25] Descriptor tables — 1 SRV each at t10-t12 space0 (clustered lighting)
    static D3D12_DESCRIPTOR_RANGE clusterRanges[kClusterSRVSlotCount] = {};
    for (UINT ci = 0; ci < kClusterSRVSlotCount; ++ci)
    {
        clusterRanges[ci].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        clusterRanges[ci].NumDescriptors                    = 1;
        clusterRanges[ci].BaseShaderRegister                = 13 + ci; // t13, t14, t15 (after shadow t9-t11)
        clusterRanges[ci].RegisterSpace                     = 0;
        clusterRanges[ci].OffsetInDescriptorsFromTableStart = 0;

        params[kClusterSRVSlotBase + ci].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kClusterSRVSlotBase + ci].DescriptorTable.NumDescriptorRanges = 1;
        params[kClusterSRVSlotBase + ci].DescriptorTable.pDescriptorRanges   = &clusterRanges[ci];
        params[kClusterSRVSlotBase + ci].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [26] Descriptor table — 1 SRV at t16 space0 (NPR ramp texture)
    static D3D12_DESCRIPTOR_RANGE rampRange = {};
    rampRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rampRange.NumDescriptors                    = 1;
    rampRange.BaseShaderRegister                = 16; // t16
    rampRange.RegisterSpace                     = 0;
    rampRange.OffsetInDescriptorsFromTableStart = 0;

    params[kRampTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRampTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kRampTexSlot].DescriptorTable.pDescriptorRanges   = &rampRange;
    params[kRampTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [28] Descriptor table — 1 SRV at t12 space0 (MaterialBuffer for per-material NPR params)
    static D3D12_DESCRIPTOR_RANGE materialBufRange = {};
    materialBufRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialBufRange.NumDescriptors                    = 1;
    materialBufRange.BaseShaderRegister                = 12; // t12
    materialBufRange.RegisterSpace                     = 0;
    materialBufRange.OffsetInDescriptorsFromTableStart = 0;

    params[kMaterialBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kMaterialBufSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kMaterialBufSlot].DescriptorTable.pDescriptorRanges   = &materialBufRange;
    params[kMaterialBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [27] Descriptor table — unbounded SRVs at t0 space2 (bindless texture array)
    static D3D12_DESCRIPTOR_RANGE bindlessTexRange = {};
    bindlessTexRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessTexRange.NumDescriptors                    = kMaxBindlessTextures;
    bindlessTexRange.BaseShaderRegister                = 0; // t0
    bindlessTexRange.RegisterSpace                     = 2; // space2
    bindlessTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessTexSlot].DescriptorTable.pDescriptorRanges   = &bindlessTexRange;
    params[kBindlessTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [29] Descriptor table — 1 SRV at t17 space0 (GBuffer emissive for deferred lighting)
    static D3D12_DESCRIPTOR_RANGE emissiveRange = {};
    emissiveRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    emissiveRange.NumDescriptors                    = 1;
    emissiveRange.BaseShaderRegister                = 17; // t17
    emissiveRange.RegisterSpace                     = 0;
    emissiveRange.OffsetInDescriptorsFromTableStart = 0;

    params[kEmissiveSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kEmissiveSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kEmissiveSRVSlot].DescriptorTable.pDescriptorRanges   = &emissiveRange;
    params[kEmissiveSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [30] Descriptor table — 1 SRV at t10 space0 (SSAO texture from XeGTAO)
    static D3D12_DESCRIPTOR_RANGE ssaoRange = {};
    ssaoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssaoRange.NumDescriptors                    = 1;
    ssaoRange.BaseShaderRegister                = 18; // t18
    ssaoRange.RegisterSpace                     = 0;
    ssaoRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSSAOSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSSAOSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSSAOSRVSlot].DescriptorTable.pDescriptorRanges   = &ssaoRange;
    params[kSSAOSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [31] Descriptor table — 1 SRV at t19 space0 (Sky SH coefficient buffer)
    static D3D12_DESCRIPTOR_RANGE skySHRange = {};
    skySHRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    skySHRange.NumDescriptors                    = 1;
    skySHRange.BaseShaderRegister                = 19; // t19
    skySHRange.RegisterSpace                     = 0;
    skySHRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSkySHSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSkySHSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSkySHSRVSlot].DescriptorTable.pDescriptorRanges   = &skySHRange;
    params[kSkySHSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [32] Descriptor table — 1 SRV at t20 space0 (Aerial Perspective 3D LUT)
    static D3D12_DESCRIPTOR_RANGE apRange = {};
    apRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    apRange.NumDescriptors                    = 1;
    apRange.BaseShaderRegister                = 20; // t20
    apRange.RegisterSpace                     = 0;
    apRange.OffsetInDescriptorsFromTableStart = 0;

    params[kAerialPerspSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kAerialPerspSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kAerialPerspSlot].DescriptorTable.pDescriptorRanges   = &apRange;
    params[kAerialPerspSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [33] Descriptor table — 1 SRV at t21 space0 (SpotShadowPass atlas).
    // Texture2DArray<float> D32; sampled with the existing shadow comparison
    // sampler (s2) for PCF inside the Lighting / Volumetric-fog shaders.
    static D3D12_DESCRIPTOR_RANGE spotShadowAtlasRange = {};
    spotShadowAtlasRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    spotShadowAtlasRange.NumDescriptors   = 1;
    spotShadowAtlasRange.BaseShaderRegister = 21; // t21
    spotShadowAtlasRange.RegisterSpace      = 0;
    spotShadowAtlasRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowAtlasSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowAtlasSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowAtlasSlot].DescriptorTable.pDescriptorRanges   = &spotShadowAtlasRange;
    params[kSpotShadowAtlasSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [34] Descriptor table — 1 SRV at t22 space0 (SpotShadowPass per-slice
    // viewProj buffer, StructuredBuffer<float4x4>). Indexed by GPULight::
    // shadowSliceIdx so the consumer shader can transform worldPos into the
    // light's shadow-map space and do a single SampleCmpLevelZero.
    static D3D12_DESCRIPTOR_RANGE spotShadowVPRange = {};
    spotShadowVPRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    spotShadowVPRange.NumDescriptors   = 1;
    spotShadowVPRange.BaseShaderRegister = 22; // t22
    spotShadowVPRange.RegisterSpace      = 0;
    spotShadowVPRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowVPSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowVPSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowVPSlot].DescriptorTable.pDescriptorRanges   = &spotShadowVPRange;
    params[kSpotShadowVPSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [35] Root CBV — per-material custom cbuffer (Phase E). Custom GBuffer
    // pixel shaders may declare `cbuffer Foo : register(b8, space0) {...}`;
    // Renderer packs MaterialComponent::customParams into an upload-heap ring
    // each frame and binds the GPU VA here. Standard shaders don't declare
    // b8 so leaving this slot unbound when not in use is a no-op.
    params[kCustomMatCBVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kCustomMatCBVSlot].Descriptor.ShaderRegister = 8; // b8
    params[kCustomMatCBVSlot].Descriptor.RegisterSpace  = 0;
    params[kCustomMatCBVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [36] Descriptor table — up to kCustomMatTexCount SRVs at t0-t3 space3
    // (Phase F). Custom pixel shaders may declare `Texture2D MyTex :
    // register(tN, space3)` and bind them by name; Renderer copies each
    // reflected texture's SRV into a per-material ring slot each frame.
    // Unused slots get a default white descriptor so sampling never reads
    // random heap memory.
    static D3D12_DESCRIPTOR_RANGE customMatTexRange = {};
    customMatTexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    customMatTexRange.NumDescriptors   = kCustomMatTexCount;
    customMatTexRange.BaseShaderRegister = 0;  // t0
    customMatTexRange.RegisterSpace      = 3;  // space3
    customMatTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kCustomMatTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kCustomMatTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kCustomMatTexSlot].DescriptorTable.pDescriptorRanges   = &customMatTexRange;
    params[kCustomMatTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [37] Descriptor table — 1 SRV at t23 space0 (reflection probe TextureCubeArray).
    // Renderer keeps this resource persistently allocated, so the binding is
    // always satisfied even with zero probes in the world.
    static D3D12_DESCRIPTOR_RANGE reflectionProbeArrayRange = {};
    reflectionProbeArrayRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeArrayRange.NumDescriptors   = 1;
    reflectionProbeArrayRange.BaseShaderRegister = 23; // t23
    reflectionProbeArrayRange.RegisterSpace      = 0;
    reflectionProbeArrayRange.OffsetInDescriptorsFromTableStart = 0;

    params[kReflectionProbeArraySlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeArraySlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeArraySlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeArrayRange;
    params[kReflectionProbeArraySlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [38] Descriptor table — 1 SRV at t24 space0 (StructuredBuffer<GPUReflectionProbe>).
    static D3D12_DESCRIPTOR_RANGE reflectionProbeBufferRange = {};
    reflectionProbeBufferRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeBufferRange.NumDescriptors   = 1;
    reflectionProbeBufferRange.BaseShaderRegister = 24; // t24
    reflectionProbeBufferRange.RegisterSpace      = 0;
    reflectionProbeBufferRange.OffsetInDescriptorsFromTableStart = 0;

    params[kReflectionProbeBufferSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeBufferSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeBufferSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeBufferRange;
    params[kReflectionProbeBufferSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [39] Descriptor table — 1 SRV at t25 space0 (StructuredBuffer<ProbeGridEntry>)
    static D3D12_DESCRIPTOR_RANGE reflectionProbeGridRange = {};
    reflectionProbeGridRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeGridRange.NumDescriptors   = 1;
    reflectionProbeGridRange.BaseShaderRegister = 25; // t25
    reflectionProbeGridRange.RegisterSpace      = 0;
    reflectionProbeGridRange.OffsetInDescriptorsFromTableStart = 0;
    params[kReflectionProbeGridSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeGridSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeGridSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeGridRange;
    params[kReflectionProbeGridSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [40] Descriptor table — 1 SRV at t26 space0 (StructuredBuffer<uint> probe index list)
    static D3D12_DESCRIPTOR_RANGE reflectionProbeIndexRange = {};
    reflectionProbeIndexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    reflectionProbeIndexRange.NumDescriptors   = 1;
    reflectionProbeIndexRange.BaseShaderRegister = 26; // t26
    reflectionProbeIndexRange.RegisterSpace      = 0;
    reflectionProbeIndexRange.OffsetInDescriptorsFromTableStart = 0;
    params[kReflectionProbeIndexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kReflectionProbeIndexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kReflectionProbeIndexSlot].DescriptorTable.pDescriptorRanges   = &reflectionProbeIndexRange;
    params[kReflectionProbeIndexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [41] Descriptor table — 1 SRV at t27 space0 (SSR trace result, 1-frame
    // latent). Lighting.ps dampens its own specIBL by (1 - ssrConf) so the
    // composite step can additively write SSR contribution without double-
    // counting the specular term.
    static D3D12_DESCRIPTOR_RANGE ssrResultRange = {};
    ssrResultRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ssrResultRange.NumDescriptors   = 1;
    ssrResultRange.BaseShaderRegister = 27; // t27
    ssrResultRange.RegisterSpace      = 0;
    ssrResultRange.OffsetInDescriptorsFromTableStart = 0;
    params[kSSRResultSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSSRResultSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSSRResultSlot].DescriptorTable.pDescriptorRanges   = &ssrResultRange;
    params[kSSRResultSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [42..45] DDGI integration — multi-volume. Each table-typed slot below holds
    // DDGI_MAX_VOLUMES contiguous SRVs; Lighting.ps iterates `vi < ddgiVolumeCount`
    // (uniform from LightCB) to consume only the active subset.
    static D3D12_DESCRIPTOR_RANGE ddgiVolBufRange{};
    ddgiVolBufRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiVolBufRange.NumDescriptors   = 1;
    ddgiVolBufRange.BaseShaderRegister = 28; // t28
    ddgiVolBufRange.RegisterSpace      = 0;
    params[kDDGIVolumeBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIVolumeBufSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIVolumeBufSlot].DescriptorTable.pDescriptorRanges   = &ddgiVolBufRange;
    params[kDDGIVolumeBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiProbeSHRange{};
    ddgiProbeSHRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiProbeSHRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiProbeSHRange.BaseShaderRegister = 29; // t29..t32
    ddgiProbeSHRange.RegisterSpace      = 0;
    params[kDDGIProbeSHSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIProbeSHSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIProbeSHSlot].DescriptorTable.pDescriptorRanges   = &ddgiProbeSHRange;
    params[kDDGIProbeSHSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiDepthRange{};
    ddgiDepthRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiDepthRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiDepthRange.BaseShaderRegister = 33; // t33..t36
    ddgiDepthRange.RegisterSpace      = 0;
    params[kDDGIDepthSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIDepthSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIDepthSlot].DescriptorTable.pDescriptorRanges   = &ddgiDepthRange;
    params[kDDGIDepthSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    static D3D12_DESCRIPTOR_RANGE ddgiProbeDataRange{};
    ddgiProbeDataRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ddgiProbeDataRange.NumDescriptors   = kDDGIMaxVolumes;
    ddgiProbeDataRange.BaseShaderRegister = 37; // t37..t40
    ddgiProbeDataRange.RegisterSpace      = 0;
    params[kDDGIProbeDataSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kDDGIProbeDataSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kDDGIProbeDataSlot].DescriptorTable.pDescriptorRanges   = &ddgiProbeDataRange;
    params[kDDGIProbeDataSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters     = totalParams;
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 0;
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE; // No IA layout — PVF

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        const char* msg = errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "";
        LOG_ERROR("CreateDefaultRootSignature failed: %s", msg);
        ThrowIfFailed(hr);
    }

    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    return rs;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> GraphicsDX12::CreateComputeRootSignature()
{
    // Full slot map lives in the GraphicsDX12.h class-banner comment.
    CD3DX12_ROOT_PARAMETER1 params[18];
    params[0].InitAsConstantBufferView(0, 2, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                                       D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 srvRanges[5];
    srvRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2);
    srvRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 2);
    srvRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 2);
    srvRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 2);
    srvRanges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 2); // morph deltas
    CD3DX12_DESCRIPTOR_RANGE1 srvRange5;
    srvRange5.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5, 2);  // morph weights
    params[1].InitAsDescriptorTable(1, &srvRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[2].InitAsDescriptorTable(1, &srvRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[3].InitAsDescriptorTable(1, &srvRanges[2], D3D12_SHADER_VISIBILITY_ALL);
    params[7].InitAsDescriptorTable(1, &srvRanges[3], D3D12_SHADER_VISIBILITY_ALL);
    params[8].InitAsDescriptorTable(1, &srvRanges[4], D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 uavRanges[2];
    uavRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2);
    uavRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 2);
    params[4].InitAsDescriptorTable(1, &uavRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[5].InitAsDescriptorTable(1, &uavRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[6].InitAsDescriptorTable(1, &srvRange5,    D3D12_SHADER_VISIBILITY_ALL);

    // [9], [10] — mirror of graphics [8], [9] for PVF under compute. Only
    // SceneVoxelize binds them today; unused slots are legal.
    static CD3DX12_DESCRIPTOR_RANGE1 instanceRange;
    static CD3DX12_DESCRIPTOR_RANGE1 meshDescRange;
    instanceRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); // t0 space0
    meshDescRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0); // t1 space0
    params[9 ].InitAsDescriptorTable(1, &instanceRange, D3D12_SHADER_VISIBILITY_ALL);
    params[10].InitAsDescriptorTable(1, &meshDescRange, D3D12_SHADER_VISIBILITY_ALL);

    // [11] — bindless vertex/index buffer array, identical layout to the
    // graphics root sig so pvf_fetch.hlsli "just works" under compute.
    CD3DX12_DESCRIPTOR_RANGE1 bindlessRange;
    bindlessRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                       kMaxBindlessBuffers, 0, 1,
                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[11].InitAsDescriptorTable(1, &bindlessRange, D3D12_SHADER_VISIBILITY_ALL);

    // [12] — SpotShadowPass atlas (Texture2DArray<float>) at t6 space2.
    // [13] — per-slice viewProj StructuredBuffer at t7 space2.
    // These let FroxelLightInject sample per-light shadow maps so spot
    // beams that get blocked by walls don't light the fog behind them.
    static CD3DX12_DESCRIPTOR_RANGE1 spotShadowAtlasRangeC;
    static CD3DX12_DESCRIPTOR_RANGE1 spotShadowVPRangeC;
    spotShadowAtlasRangeC.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 2); // t6 space2
    spotShadowVPRangeC   .Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7, 2); // t7 space2
    params[12].InitAsDescriptorTable(1, &spotShadowAtlasRangeC, D3D12_SHADER_VISIBILITY_ALL);
    params[13].InitAsDescriptorTable(1, &spotShadowVPRangeC,    D3D12_SHADER_VISIBILITY_ALL);

    // [14..16] — u2 / u3 / u4 space2 (extra UAV slots for XeGTAO prefilter).
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU2;
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU3;
    static CD3DX12_DESCRIPTOR_RANGE1 uavRangeU4;
    uavRangeU2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 2);  // u2 space2
    uavRangeU3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 2);  // u3 space2
    uavRangeU4.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 2);  // u4 space2
    params[14].InitAsDescriptorTable(1, &uavRangeU2, D3D12_SHADER_VISIBILITY_ALL);
    params[15].InitAsDescriptorTable(1, &uavRangeU3, D3D12_SHADER_VISIBILITY_ALL);
    params[16].InitAsDescriptorTable(1, &uavRangeU4, D3D12_SHADER_VISIBILITY_ALL);

    // [17] — bindless Texture2D[] at t0 space3. The GPU handle passed at
    // bind time is the same heap-region base as graphics kBindlessTexSlot;
    // the shader just sees them at a different register/space so compute
    // and graphics shaders don't need identical HLSL layouts. DESCRIPTORS_
    // VOLATILE matches the graphics side — new texture allocations may mutate
    // the table between recordings.
    static CD3DX12_DESCRIPTOR_RANGE1 bindlessTex2DRange;
    bindlessTex2DRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                            kMaxBindlessTextures, 0, 3,
                            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[17].InitAsDescriptorTable(1, &bindlessTex2DRange, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2]{};

    // s0 space2 — plain linear-clamp (for Sample / SampleLevel).
    samplers[0].Init(0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 16, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z (ignored: non-comparison filter)
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[0].RegisterSpace = 2;

    // s1 space2 — comparison sampler (PCF) for CSM shadow sampling in the
    // volumetric-fog light-inject compute shader. Without this the shader's
    // SampleCmpLevelZero call was paired with a non-comparison sampler and
    // silently returned garbage → no visible god rays.
    samplers[1].Init(1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 0, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z: lit when receiver z >= SM z
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[1].RegisterSpace = 2;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(18, params, 2, samplers,
                    D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc,
                     D3D_ROOT_SIGNATURE_VERSION_1_1,
                     &serialized, &error);
    if (FAILED(hr))
    {
        if (error) LOG_ERROR("CreateComputeRootSignature: %s",
                             static_cast<char*>(error->GetBufferPointer()));
        return nullptr;
    }

    ComPtr<ID3D12RootSignature> rs;
    hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                       serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&rs));
    if (FAILED(hr)) { LOG_ERROR("CreateComputeRootSignature: CreateRootSignature failed"); return nullptr; }
    return rs;
}

// ===========================================================================
// Construction / destruction
// ===========================================================================

GraphicsDX12::GraphicsDX12(HWND hWnd, UINT width, UINT height)
    : m_hWnd(hWnd), m_width(width), m_height(height)
{
    LOG_INFO("GraphicsDX12 constructor");
    LoadPipeline(hWnd, width, height);
    LoadAssets();
    // ImGui init lives in the editor (EditorImGuiBridge::Attach) — engine is
    // UI-agnostic. Game builds never touch ImGui at all.
    m_gpuProfiler.Init(m_device.Get(),
                       m_queues[0].Get(),  // GRAPHICS
                       m_queues[1].Get(),  // COMPUTE
                       m_queues[2].Get()); // COPY

    // Pre-reserve the command-list pointer table so that concurrent calls to
    // BeginCommandList (from render workers) never trigger a vector reallocation
    // while another thread is reading m_commandLists[id] in GetPoolEntry.
    // push_back only appends a pointer; existing entries are never relocated.
    // 64 slots is far more than any single frame will use.
    m_commandLists.reserve(64);
}

GraphicsDX12::~GraphicsDX12()
{
    // Ensure GPU finished before releasing D3D12 objects.
    if (m_device && m_fenceEvent)
    {
        try { WaitForPreviousFrame(); } catch (...) {}
    }

    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }

    // Release the video backend before any other DX12 objects — its
    // destructor CPU-blocks on the video queue + drops decoder ComPtrs.
    // Doing it here (instead of letting member-order destruction handle it)
    // keeps the ID3D12Device alive while the video resources unwind.
    m_videoBackend.reset();

    // Release all resource pools + command-list pool *before* live-object reporting.
    // Otherwise ReportLiveDeviceObjects() will list objects that are only still
    // alive until member-destructors run at the end of this destructor body.

    // HDR render target + its descriptor allocations.
    m_hdrRtvAllocation.Free();
    m_hdrSrvAllocation.Free();
    m_hdrUavAllocation.Free();
    m_hdrRenderTarget.Reset();

    // Explicitly destroy command-list pool entries constructed via placement-new.
    // BlockAllocator itself does not call T destructors automatically.
    for (CommandList_DX12* p : m_commandLists)
    {
        if (p) p->~CommandList_DX12();
    }
    m_commandLists.clear();
    m_commandListCount = 0;
    m_cmdAllocator.blocks.clear();
    m_cmdAllocator.freeList.clear();

    // Release CPU-side pools that hold ComPtr resources/pso/root signatures.
    m_bufferPool.clear();
    m_texturePool.clear();
    m_shaderPool.clear();
    m_psoPool.clear();
    m_samplerPool.clear();
    m_bufferFreeList.clear();
    m_textureFreeList.clear();

    // Release descriptor heap allocators (heaps are inside DescriptorHeapAllocator).
    m_rtvAllocator = {};
    m_cbvSrvUavAllocator = {};
    m_dsvAllocator = {};
    m_samplerAllocator = {};

    // Release upload-only staging objects and synchronization primitives.
    m_uploadAllocator.Reset();
    m_commandList.Reset();
    m_fence.Reset();
    for (auto& f : m_queueFences) f.Reset();
    m_scRTVHeap.Reset();

    // Pipeline library bytes & ComPtr.
    m_psoLibraryData.clear();
    m_psoLibrary.Reset();

    // Enforce correct DXGI release order in the destructor body so that
    // member-destructor ordering (reverse-declaration) doesn't matter:
    //   render targets → swap chain (after SetFullscreenState) → queues
    // This prevents the DXGI "process terminating" live-object warning.
    for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
    if (m_swapChain)
    {
        m_swapChain->SetFullscreenState(FALSE, nullptr);
        m_swapChain.Reset();
    }
    for (auto& q : m_queues) q.Reset();

#ifdef _DEBUG
    // Report live objects while the message callback is still registered so
    // output reaches the log.  Use IGNORE_INTERNAL to filter debug-layer
    // bookkeeping objects and see only application-level leaks.
    if (m_device)
    {
        ComPtr<ID3D12DebugDevice1> debugDevice1;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice1))) && debugDevice1)
        {
            debugDevice1->ReportLiveDeviceObjects(
                D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
        else
        {
            ComPtr<ID3D12DebugDevice> debugDevice;
            if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice))) && debugDevice)
                debugDevice->ReportLiveDeviceObjects(
                    D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
    }

    if (m_d3d12MessageCallbackCookie != 0 && m_device)
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
            infoQueue->UnregisterMessageCallback(m_d3d12MessageCallbackCookie);
        m_d3d12MessageCallbackCookie = 0;
    }
#endif
    // Member ComPtr destructors release the rest (device, factory, heaps, pools).
}


// ===========================================================================
// Pool helpers
// ===========================================================================

CommandList_DX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd)
{
    assert(cmd.internal_id < m_commandListCount && "Invalid CommandList handle");
    return *m_commandLists[cmd.internal_id];
}

const CommandList_DX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd) const
{
    assert(cmd.internal_id < m_commandListCount && "Invalid CommandList handle");
    return *m_commandLists[cmd.internal_id];
}

void GraphicsDX12::AddCommandListDependency(RHI::CommandList waiter,
                                             RHI::CommandList dependency)
{
    assert(waiter.internal_id     < m_commandListCount && "waiter handle invalid");
    assert(dependency.internal_id < m_commandListCount && "dependency handle invalid");
    assert(waiter.internal_id != dependency.internal_id && "self-dependency");
    m_commandLists[waiter.internal_id]->wait_for.push_back(dependency.internal_id);
}

// ===========================================================================
// DX12-specific accessors
// ===========================================================================

ID3D12GraphicsCommandList* GraphicsDX12::GetNativeCommandList(RHI::CommandList cmd) const
{
    return GetPoolEntry(cmd).GetCommandList();
}
ID3D12Device*              GraphicsDX12::GetDevice()            const { return m_device.Get(); }

ID3D12Device5* GraphicsDX12::GetDevice5() const
{
    if (!m_dxrSupported) return nullptr;
    // QI on demand — the cost is one COM ref bump and is amortised by the
    // fact that DDGI calls this once at Compile() to cache the pointer.
    ID3D12Device5* dev5 = nullptr;
    if (FAILED(m_device.Get()->QueryInterface(IID_PPV_ARGS(&dev5))))
        return nullptr;
    dev5->Release(); // caller does not own ref; pointer lifetime tied to m_device
    return dev5;
}
DescriptorHeapAllocator&   GraphicsDX12::GetRtvAllocator()            { return m_rtvAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetCbvSrvUavAllocator()      { return m_cbvSrvUavAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetDsvAllocator()             { return m_dsvAllocator; }

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureDsvCpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid() || tex.handle_id >= static_cast<uint32_t>(m_texturePool.size()))
        return D3D12_CPU_DESCRIPTOR_HANDLE{};
    return m_texturePool[tex.handle_id].dsv.GetCpuHandle();
}

ID3D12Resource* GraphicsDX12::GetBufferResource(const RHI::GPUBuffer& buffer) const
{
    if (!buffer.IsValid() || buffer.handle_id >= m_bufferPool.size()) return nullptr;
    return m_bufferPool[buffer.handle_id].resource.Get();
}

// ---------------------------------------------------------------------------
uint64_t GraphicsDX12::CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                                   uint32_t            count)
{
    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(count);
    if (!alloc.IsValid())
    {
        LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: descriptor allocation failed");
        return 0;
    }

    const UINT descSize = m_cbvSrvUavAllocator.GetDescriptorSize();

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!textures[i].IsValid()) continue;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = alloc.GetCpuHandle();
        cpuSlot.ptr += static_cast<SIZE_T>(i) * descSize;

        ID3D12Resource* resource = GetTextureResource(textures[i]);
        if (resource)
            m_device->CreateShaderResourceView(resource, &srvDesc, cpuSlot);
        else
            LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: no resource for slot %u", i);
    }

    alloc.CopyToGpu();
    const uint64_t gpuHandle = alloc.GetGpuHandle().ptr;
    m_descriptorTablePool[gpuHandle] = std::move(alloc);
    LOG_INFO("GraphicsDX12::CreateDepthTextureSRVTable: handle = %llu (%u entries)", gpuHandle, count);
    return gpuHandle;
}

// ---------------------------------------------------------------------------
void GraphicsDX12::FreeDescriptorTable(uint64_t gpuHandle)
{
    if (!gpuHandle) return;
    auto it = m_descriptorTablePool.find(gpuHandle);
    if (it != m_descriptorTablePool.end())
    {
        // Defer: GPU may still reference this table via in-flight draws.
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        m_deferredRelease[m_frameIndex].descriptors.push_back(std::move(it->second));
        m_descriptorTablePool.erase(it);
    }
}

ID3D12Resource* GraphicsDX12::GetTextureResource(const RHI::Texture& texture) const
{
    if (!texture.IsValid() || texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return nullptr;
    return m_texturePool[texture.handle_id].resource.Get();
}

// ---------------------------------------------------------------------------
// Descriptor-table fill helpers (multi-volume DDGI). See header for usage.
// ---------------------------------------------------------------------------
void GraphicsDX12::CreateStructuredBufferSRVAtCpu(const RHI::GPUBuffer& buf,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetBufferResource(buf);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN; // structured
    const uint32_t stride           = buf.desc.stride > 0 ? buf.desc.stride : 4;
    srvd.Buffer.NumElements         = static_cast<UINT>(buf.desc.size / stride);
    srvd.Buffer.StructureByteStride = stride;
    m_device->CreateShaderResourceView(res, &srvd, dst);
}

void GraphicsDX12::CreateTexture2DSRVAtCpu(const RHI::Texture& tex,
                                           D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetTextureResource(tex);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = ToDxgiFormat(tex.desc.format);
    srvd.Texture2D.MipLevels        = tex.desc.mip_levels > 0 ? tex.desc.mip_levels : 1;
    srvd.Texture2D.MostDetailedMip  = 0;
    m_device->CreateShaderResourceView(res, &srvd, dst);
}

void GraphicsDX12::CreateNullStructuredBufferSRVAtCpu(uint32_t stride,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN;
    srvd.Buffer.NumElements         = 1;
    srvd.Buffer.StructureByteStride = stride > 0 ? stride : 4;
    m_device->CreateShaderResourceView(nullptr, &srvd, dst);
}

void GraphicsDX12::CreateNullTexture2DSRVAtCpu(DXGI_FORMAT format,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = format;
    srvd.Texture2D.MipLevels        = 1;
    m_device->CreateShaderResourceView(nullptr, &srvd, dst);
}

// ===========================================================================
// Pipeline setup
// ===========================================================================

void GraphicsDX12::LoadPipeline(HWND hWnd, UINT width, UINT height)
{
    LOG_INFO("LoadPipeline begin");

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            LOG_INFO("D3D12 debug layer enabled — messages will be forwarded to Logger");
        }
    }
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; DXGI_ERROR_NOT_FOUND != m_factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)); ++i)
    {
        DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        LOG_INFO("Trying adapter: %ls", desc.Description);
        bool ok = false;
        for (auto fl : featureLevels)
            if (SUCCEEDED(D3D12CreateDevice(nullptr, fl, IID_PPV_ARGS(&m_device)))) { ok = true; break; }
        if (ok) break;
    }

    // Create one command queue per queue type (GRAPHICS / COMPUTE / COPY)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[0])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[1])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[2])));
    }

#ifdef _DEBUG
    // Forward D3D12 debug layer messages to the app's Log system
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            struct D3D12LogCallback
            {
                static void __stdcall Callback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                    D3D12_MESSAGE_ID, LPCSTR pDescription, void*)
                {
                    LogLevel level = LogLevel::Info;
                    switch (severity)
                    {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                    case D3D12_MESSAGE_SEVERITY_ERROR:   level = LogLevel::Error;   break;
                    case D3D12_MESSAGE_SEVERITY_WARNING: level = LogLevel::Warning; break;
                    default:                             level = LogLevel::Info;    break;
                    }
                    Logger::Get().Log(level, "D3D12", 0, "%s", pDescription ? pDescription : "");
                }
            };
            if (SUCCEEDED(infoQueue->RegisterMessageCallback(
                    &D3D12LogCallback::Callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &m_d3d12MessageCallbackCookie)))
                LOG_INFO("D3D12 debug messages forwarded to Logger");
        }
    }
#endif

    // Check tearing (variable refresh rate) support.
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                          &allowTearing, sizeof(allowTearing));
        m_tearingSupported = (allowTearing == TRUE);
        LOG_INFO("GraphicsDX12: tearing support = %s", m_tearingSupported ? "YES" : "NO");
    }

    // Mesh-shader capability (Options7). Tier_1 is the only non-NotSupported
    // value today. Terrain / any future MS pass checks SupportsMeshShader()
    // and falls back gracefully when the runtime / GPU lacks support.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                    &opts7, sizeof(opts7))))
        {
            m_meshShaderSupported = (opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
        }
        LOG_INFO("GraphicsDX12: mesh shader support = %s",
                 m_meshShaderSupported ? "YES (Tier_1+)" : "NO");
    }

    // DXR raytracing capability (Options5). DDGI probe trace requires Tier_1_0.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                    &opts5, sizeof(opts5))))
        {
            m_dxrSupported = (opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
        }
        LOG_INFO("GraphicsDX12: DXR support = %s",
                 m_dxrSupported ? "YES (Tier_1_0+)" : "NO");
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount = FrameCount;
    scd.Width = width; scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    if (m_tearingSupported)
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> sc;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hWnd, &scd, nullptr, nullptr, &sc));
    ThrowIfFailed(m_factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(sc.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Swap-chain RTV heap (not managed by allocator)
    D3D12_DESCRIPTOR_HEAP_DESC rhd = {};
    rhd.NumDescriptors = FrameCount;
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&m_scRTVHeap)));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Managed descriptor heap allocators
    // RTV / DSV: CPU-only (gpuStatic=0, gpuDynamic=0)
    m_rtvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256,  0, 0);
    m_dsvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,  64,  0, 0);
    // CBV_SRV_UAV: 4096 CPU staging, 65536 GPU static, 934464 GPU dynamic  (total 1 000 000 GPU)
    m_cbvSrvUavAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, 65536, 934464);
    // Sampler: 256 CPU staging, 2048 GPU static, 0 dynamic
    m_samplerAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, MaxSamplers, 0);

    // Allocate bindless texture table (contiguous SRV block for all textures).
    m_bindlessTexTable = m_cbvSrvUavAllocator.Allocate(kMaxBindlessTextures);
    if (m_bindlessTexTable.IsValid())
    {
        // Fill with null SRVs (Texture2D, format=UNKNOWN) to avoid GPU faults on unbound slots.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels     = 1;
        UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH = m_bindlessTexTable.GetGpuCpuHandle();
        for (uint32_t i = 0; i < kMaxBindlessTextures; ++i)
        {
            m_device->CreateShaderResourceView(nullptr, &nullSrv, cpuH);
            cpuH.ptr += descInc;
        }
        LOG_SUCCESS("GraphicsDX12: bindless texture table allocated (%u slots)", kMaxBindlessTextures);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
        m_renderTargets[n]->SetName(bbName.c_str());
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvH);
        rtvH.ptr += m_rtvDescriptorSize;
    }

    // Dedicated upload allocator (used by CreateBuffer / CreateTexture staging)
    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_uploadAllocator)));

    CreateHdrRenderTarget(width, height);
    LOG_SUCCESS("LoadPipeline finished");
}

void GraphicsDX12::LoadAssets()
{
    // Upload command list — stays open so CreateBuffer/CreateTexture can record into it.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    // Created open; leave it open for immediate staging use.

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    // Per-queue GPU-GPU cross-queue synchronization fences
    for (UINT q = 0; q < 3; ++q)
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_queueFences[q])));
        m_queueFenceValues[q] = 0;
    }

    m_viewport    = { 0,0, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(m_width),  static_cast<LONG>(m_height) };

    // Build the shared root signature once — reused by every CreatePipelineState call.
    m_defaultRootSignature = CreateDefaultRootSignature();

    m_computeRootSignature = CreateComputeRootSignature();
    if (!m_computeRootSignature)
    { LOG_ERROR("GraphicsDX12: failed to create compute root signature"); }
    else
    { LOG_SUCCESS("GraphicsDX12: compute root signature ready"); }

    // ---- Command Signature for ExecuteIndirect (root constants + DrawInstanced) ---
    {
        D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
        // Arg 0: 4 root constants at param 0
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant.RootParameterIndex = kRootConstantsSlot;
        args[0].Constant.DestOffsetIn32BitValues = 0;
        args[0].Constant.Num32BitValuesToSet = 4;
        // Arg 1: DrawInstanced
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

        D3D12_COMMAND_SIGNATURE_DESC csDesc = {};
        csDesc.ByteStride = 32; // sizeof(IndirectDrawCommand) = 4*4 root const + 4*4 draw args = 32
        csDesc.NumArgumentDescs = 2;
        csDesc.pArgumentDescs = args;

        HRESULT hr = m_device->CreateCommandSignature(
            &csDesc, m_defaultRootSignature.Get(),
            IID_PPV_ARGS(&m_indirectCommandSignature));
        if (FAILED(hr))
            LOG_ERROR("GraphicsDX12: CreateCommandSignature failed (hr=0x%08X)", static_cast<unsigned>(hr));
        else
            LOG_SUCCESS("GraphicsDX12: indirect command signature ready (stride=%u)", csDesc.ByteStride);
    }

    // Pre-reserve resource pools so that push_back never reallocates while another
    // thread is holding a reference obtained from an earlier Create* call.
    // m_psoPool is a std::deque (stable refs on push_back regardless of size) —
    // see GraphicsDX12.h for rationale. No reserve call on deque.
    m_bufferPool.reserve(4096);
    m_texturePool.reserve(2048);
    m_shaderPool.reserve(512);
}

void GraphicsDX12::WaitForPreviousFrame()
{
    // Sync GRAPHICS queue (CPU-GPU frame pacing fence).
    // Pre-increment so the signaled value is strictly > any previously signaled one —
    // matches EndFrame's pre-increment to keep m_fence values monotonically unique.
    const UINT64 fence = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fence));
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // Sync COMPUTE and COPY queues via their cross-queue fences (reuse m_fenceEvent)
    for (UINT q = 1; q <= 2; ++q)
    {
        if (m_queueFenceValues[q] == 0) continue;
        if (m_queueFences[q]->GetCompletedValue() < m_queueFenceValues[q])
        {
            ThrowIfFailed(m_queueFences[q]->SetEventOnCompletion(
                m_queueFenceValues[q], m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    // GPU is fully idle — drain every deferred-release slot (not just the
    // current backbuffer's), so Resize / destructors can safely Release resources.
    ProcessAllDeferredReleases();

    // GPU is fully idle, so every fence-keyed entry's target value must have
    // completed too. Drain them so callers (e.g. ~GraphicsDX12, Resize) see
    // a fully-empty release queue.
    ProcessFenceKeyedReleases();

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
void GraphicsDX12::ProcessDeferredReleases(uint32_t idx)
{
    DeferredReleaseList local;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        if (idx >= FrameCount) return;
        local = std::move(m_deferredRelease[idx]);
        m_deferredRelease[idx] = {};
    }
    // Free descriptors (returns slots to heap free lists for reuse next frame).
    for (auto& d : local.descriptors) d.Free();
    // ComPtr destructors release resources when 'local' goes out of scope.
    // Return pool slots to their free lists.
    std::lock_guard<std::mutex> lk(m_resourceMutex);
    for (uint32_t s : local.bufferSlots)  m_bufferFreeList.push_back(s);
    if (!local.textureSlots.empty())
    {
        // Null each freed texture's bindless-table slot before recycling it.
        // CreateTexture CopyDescriptorsSimple'd the texture's SRV into the
        // GPU-visible bindless heap at slot [handle_id]; DestroyTexture frees
        // only the source DescriptorAllocation, leaving that COPY dangling at a
        // released resource. A shader still holding the recycled bindless index
        // would then sample a dead descriptor — or, if the slot is later reused
        // by a non-SRV (RTV/DSV/UAV-only) texture, the stale SRV survives
        // indefinitely. Nulling here is safe: this drains FrameCount frames
        // after Destroy, so the GPU is provably done with the slot. Symmetric
        // with the null-fill at Init. Serialized with CreateTexture's bindless
        // write via m_resourceMutex (held here).
        const bool tableValid = m_bindlessTexTable.IsValid();
        const UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE base = tableValid
            ? m_bindlessTexTable.GetGpuCpuHandle() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        for (uint32_t s : local.textureSlots)
        {
            if (tableValid && s < kMaxBindlessTextures)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst = base;
                dst.ptr += static_cast<SIZE_T>(s) * descInc;
                m_device->CreateShaderResourceView(nullptr, &nullSrv, dst);
            }
            m_textureFreeList.push_back(s);
        }
    }
}

void GraphicsDX12::ProcessAllDeferredReleases()
{
    for (uint32_t i = 0; i < FrameCount; ++i)
        ProcessDeferredReleases(i);
}

void GraphicsDX12::FlushAndWait()
{
    // Signal on the graphics queue and CPU-wait until the GPU has processed
    // all previously submitted work.  Only signals the graphics queue fence
    // (picking only uses the graphics queue).
    // Pre-increment so the signaled value is strictly > any previously signaled
    // one; otherwise it can collide with EndFrame's signal and the wait is a no-op.
    const UINT64 fence = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fence));
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}


// ===========================================================================
// HDR render target
// ===========================================================================

void GraphicsDX12::CreateHdrRenderTarget(UINT width, UINT height)
{
    if (width == 0 || height == 0) return;
    ReleaseHdrRenderTarget();

    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC   td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = width;  td.Height = height;
    td.DepthOrArraySize   = 1;      td.MipLevels = 1;
    td.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count   = 1;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                          | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // SSR composite writes
    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_R16G16B16A16_FLOAT, {0,0,0,0} };
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_hdrRenderTarget)));
    m_hdrRenderTarget->SetName(L"GraphicsDX12.HdrEditorViewport");

    m_hdrRtvAllocation = m_rtvAllocator.Allocate(1);
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{ DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2D };
    m_device->CreateRenderTargetView(m_hdrRenderTarget.Get(), &rtvd, m_hdrRtvAllocation.GetCpuHandle());

    m_hdrSrvAllocation = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_hdrRenderTarget.Get(), &srvd, m_hdrSrvAllocation.GetCpuHandle());
    m_hdrSrvAllocation.CopyToGpu();

    m_hdrUavAllocation = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_hdrRenderTarget.Get(), nullptr, &uavd,
        m_hdrUavAllocation.GetCpuHandle());
    m_hdrUavAllocation.CopyToGpu();

    m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_hdrWidth = width; m_hdrHeight = height;
}

void GraphicsDX12::ReleaseHdrRenderTarget()
{
    // Defer: GPU may still be reading m_hdrRenderTarget / its descriptors
    // from frames still in flight. Freeing now would let the descriptor slots
    // be reused and the resource destroyed while GPU work is pending.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& dr = m_deferredRelease[m_frameIndex];
    if (m_hdrRtvAllocation.IsValid()) { dr.descriptors.push_back(m_hdrRtvAllocation); m_hdrRtvAllocation = {}; }
    if (m_hdrSrvAllocation.IsValid()) { dr.descriptors.push_back(m_hdrSrvAllocation); m_hdrSrvAllocation = {}; }
    if (m_hdrUavAllocation.IsValid()) { dr.descriptors.push_back(m_hdrUavAllocation); m_hdrUavAllocation = {}; }
    if (m_hdrRenderTarget)            { dr.resources.push_back(std::move(m_hdrRenderTarget)); }
}

// ===========================================================================
// Frame lifecycle
// ===========================================================================

void GraphicsDX12::WaitForNextFrameSlot()
{
    // Idempotent within a frame — BeginFrame also calls this. Cleared by
    // BeginFrame once it's consumed the prepared slot.
    if (m_frameSlotPrepared) return;

    // Wait ONLY on the fence value from the last time this backbuffer slot
    // was used (kFrameCount frames ago) — CPU runs up to kFrameCount-1 frames
    // ahead. App calls this BEFORE the Animation phase so per-frame CPU
    // writes don't tear in-flight GPU reads.
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Wait for slot's graphics-queue fence (0 == slot never used yet — first
    // kFrameCount frames after startup).
    const uint64_t gfxFence = m_frameFenceValues[m_frameIndex];
    if (gfxFence != 0 && m_fence->GetCompletedValue() < gfxFence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(gfxFence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    // Wait for slot's compute + copy queue fences (per-slot snapshots).
    for (UINT q = 1; q <= 2; ++q)
    {
        const uint64_t qf = m_frameQueueFenceValues[m_frameIndex][q];
        if (qf == 0) continue;
        if (m_queueFences[q]->GetCompletedValue() < qf)
        {
            ThrowIfFailed(m_queueFences[q]->SetEventOnCompletion(qf, m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    m_frameSlotPrepared = true;
}

RHI::CommandList GraphicsDX12::BeginFrame()
{
    // kFrameCount-deep pipelining: per-slot allocators / deferred-release /
    // profiler readback all key off m_frameIndex so other slots stay
    // untouched. WaitForPreviousFrame() is still the hard-sync path used by
    // dtor / resize / Picking / capture helpers.
    WaitForNextFrameSlot();
    m_frameSlotPrepared = false;

    // Drain ONLY this slot's deferred-release queue. The other slots may
    // still have GPU work in flight; their queues will drain when their own
    // backbuffer comes around in kFrameCount frames.
    ProcessDeferredReleases(m_frameIndex);

    // Drain fence-keyed releases whose target fence value has completed —
    // independent of the slot-rotation cycle, so resources tied to async-
    // compute fences (DDGI) can outlive kFrameCount safely without forcing
    // a WaitForPreviousFrame stall.
    ProcessFenceKeyedReleases();

    // Read back this slot's GPU timestamps — resolved kFrameCount frames ago.
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.BeginFrame(m_frameIndex);

    // Reset the pool — this slot's entries are no longer in-flight.
    m_commandListCount = 0;

    // Open the primary command list (pool entry 0) for the GRAPHICS queue.
    RHI::CommandList primary = BeginCommandList(RHI::QUEUE_TYPE::GRAPHICS);
    // primary.internal_id == 0 guaranteed (first allocation after reset).

    auto* cmd = GetPoolEntry(primary).GetCommandList();
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    // Transition swap-chain back buffer PRESENT → RENDER_TARGET.
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);

    return primary;
}

void GraphicsDX12::EndFrame()
{
    // Resolve GPU timestamp queries before closing the primary CL.
    auto* primaryCmd = m_commandLists[0]->GetCommandList();
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.ResolveQueries(primaryCmd, m_frameIndex);

    // Transition swap-chain back buffer RENDER_TARGET → PRESENT on the primary CL.
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    primaryCmd->ResourceBarrier(1, &b);

    const uint32_t n = m_commandListCount;

    // -----------------------------------------------------------------------
    // Kahn's topological sort — determines GPU submission order.
    // An edge dep→i means: pool entry dep must execute before pool entry i.
    // -----------------------------------------------------------------------
    std::vector<int> inDegree(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t dep : m_commandLists[i]->wait_for)
            if (dep < n) ++inDegree[i];

    std::queue<uint32_t> ready;
    for (uint32_t i = 0; i < n; ++i)
        if (inDegree[i] == 0) ready.push(i);

    std::vector<uint32_t> order;
    order.reserve(n);
    while (!ready.empty())
    {
        const uint32_t cur = ready.front(); ready.pop();
        order.push_back(cur);
        // Reduce in-degree of every CL that depends on cur.
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t dep : m_commandLists[j]->wait_for)
                if (dep == cur && --inDegree[j] == 0)
                    ready.push(j);
    }

    if (order.size() != n)
    {
        LOG_ERROR("EndFrame: dependency cycle in command lists — appending remaining in pool order");
        for (uint32_t i = 0; i < n; ++i)
            if (std::find(order.begin(), order.end(), i) == order.end())
                order.push_back(i);
    }

    // -----------------------------------------------------------------------
    // Submit in topological order.
    // Same-queue ordering is guaranteed by submission sequence.
    // Cross-queue ordering is enforced via per-queue GPU fences:
    //   After each submission, signal the queue's m_queueFences entry.
    //   Before each submission, Wait() on every cross-queue dependency's fence.
    // -----------------------------------------------------------------------
    for (uint32_t idx : order)
    {
        auto& cl = *m_commandLists[idx];
        const UINT qi = static_cast<UINT>(cl.queue);

        ThrowIfFailed(cl.GetCommandList()->Close());

        // Insert GPU-side Wait() for every dependency on a different queue.
        for (uint32_t depIdx : cl.wait_for)
        {
            if (depIdx >= n) continue;
            const auto& dep = *m_commandLists[depIdx];
            if (dep.queue != cl.queue && dep.signal_fence_value != 0)
            {
                const UINT depQi = static_cast<UINT>(dep.queue);
                ThrowIfFailed(m_queues[qi]->Wait(
                    m_queueFences[depQi].Get(), dep.signal_fence_value));
            }
        }

        // Execute this command list on its queue.
        ID3D12CommandList* lists[] = { cl.commandLists[qi].Get() };
        m_queues[qi]->ExecuteCommandLists(1, lists);

        // Signal this queue's fence so later cross-queue dependents can Wait() on it.
        const uint64_t sigVal = ++m_queueFenceValues[qi];
        ThrowIfFailed(m_queues[qi]->Signal(m_queueFences[qi].Get(), sigVal));
        cl.signal_fence_value = sigVal;
    }

    // Present: VSync on → SyncInterval=1, VSync off → 0 + ALLOW_TEARING.
    auto logRemovedReason = [&](HRESULT hr)
    {
        if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_HUNG &&
            hr != DXGI_ERROR_DEVICE_RESET) return;
        HRESULT reason = m_device->GetDeviceRemovedReason();
        const char* name = "unknown";
        switch ((uint32_t)reason)
        {
        case 0x887A0001: name = "DXGI_ERROR_INVALID_CALL";       break;
        case 0x887A0005: name = "DXGI_ERROR_DEVICE_REMOVED";     break;
        case 0x887A0006: name = "DXGI_ERROR_DEVICE_HUNG";        break;
        case 0x887A0007: name = "DXGI_ERROR_DEVICE_RESET";       break;
        case 0x887A0020: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
        case 0x80070057: name = "E_INVALIDARG (likely shader / resource state)"; break;
        case 0x8007000E: name = "E_OUTOFMEMORY (GPU out of VRAM)"; break;
        }
        LOG_ERROR("Device removed: hr=0x%08X reason=0x%08X (%s) — likely DXR/DDGI bug. Inspect prior log for last command.",
                  (unsigned)hr, (unsigned)reason, name);
    };

    if (vsyncEnabled)
    {
        HRESULT hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) logRemovedReason(hr);
        ThrowIfFailed(hr);
    }
    else
    {
        UINT flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = m_swapChain->Present(0, flags);
        if (FAILED(hr)) logRemovedReason(hr);
        ThrowIfFailed(hr);
    }

    // Signal fence for this frame — next BeginFrame will wait on it.
    // EndFrame does NOT wait — only BeginFrame does (single wait per frame).
    const uint64_t fv = ++m_fenceValue;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fv));

    // Record per-backbuffer fence values for the pipelined BeginFrame wait.
    // FrameCount frames later, when this backbuffer is reused, BeginFrame will
    // wait on these values before resetting its allocators / processing the
    // deferred-release slot.
    m_frameFenceValues[m_frameIndex] = fv;
    for (UINT q = 0; q < 3; ++q)
        m_frameQueueFenceValues[m_frameIndex][q] = m_queueFenceValues[q];
}

RHI::CommandList GraphicsDX12::BeginCommandList(RHI::QUEUE_TYPE queue)
{
    // ---- Allocate a pool slot (mutex-protected) ----------------------------
    m_commandListMutex.lock();
    const uint32_t id = m_commandListCount++;
    if (id >= static_cast<uint32_t>(m_commandLists.size()))
        m_commandLists.push_back(m_cmdAllocator.Allocate());
    CommandList_DX12* clPtr = m_commandLists[id];
    m_commandListMutex.unlock();

    // ---- Reset per-frame state ---------------------------------------------
    CommandList_DX12& cl     = *clPtr;
    cl.id                    = id;
    cl.queue                 = queue;
    cl.buffer_index          = m_frameIndex;
    cl.active_pso               = nullptr;
    cl.active_rootsig_graphics  = nullptr;
    cl.active_rootsig_compute   = nullptr;
    cl.prev_pt                  = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    cl.prev_stencilref          = 0;
    cl.dirty_pso                = false;
    cl.frame_barriers.clear();
    cl.discards.clear();
    cl.wait_for.clear();
    cl.signal_fence_value = 0;

    const UINT qi = static_cast<UINT>(queue);
    const D3D12_COMMAND_LIST_TYPE listType = ToD3D12CommandListType(queue);

    // ---- Lazily create command allocators (once per slot × frame × queue) --
    for (UINT f = 0; f < FrameCount; ++f)
    {
        if (!cl.commandAllocators[f][qi])
            ThrowIfFailed(m_device->CreateCommandAllocator(
                listType, IID_PPV_ARGS(&cl.commandAllocators[f][qi])));
    }

    // ---- Lazily create the command list for this slot + queue --------------
    if (!cl.commandLists[qi])
    {
        ThrowIfFailed(m_device->CreateCommandList(
            0, listType, cl.commandAllocators[m_frameIndex][qi].Get(),
            nullptr, IID_PPV_ARGS(&cl.commandLists[qi])));
        // Close immediately so Reset() works from the second frame onward.
        ThrowIfFailed(static_cast<ID3D12GraphicsCommandList*>(
            cl.commandLists[qi].Get())->Close());

        // Debug name visible in PIX / RenderDoc.
        std::wstring name = L"cmd" + std::to_wstring(id);
        cl.commandLists[qi]->SetName(name.c_str());
    }

    // ---- Reset allocator and open the command list -------------------------
    ThrowIfFailed(cl.commandAllocators[m_frameIndex][qi]->Reset());
    ThrowIfFailed(cl.GetCommandList()->Reset(
        cl.commandAllocators[m_frameIndex][qi].Get(), nullptr));

    // ---- Bind shader-visible descriptor heaps (DIRECT + COMPUTE, not COPY) -
    if (queue != RHI::QUEUE_TYPE::COPY)
    {
        ID3D12DescriptorHeap* heaps[] = {
            m_cbvSrvUavAllocator.GetGpuHeap(),
            m_samplerAllocator.GetGpuHeap()
        };
        cl.GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    return RHI::CommandList{ id };
}

// ===========================================================================
// Present composite — non-ImGui fullscreen blit
// ===========================================================================
//
// Builds a self-contained PSO (own root sig + own shaders, no dependencies on
// the engine's default root sig or ShaderLibrary) that samples an SRV and
// writes to the currently-bound swap chain RTV. Shaders are inline strings
// compiled on first use via D3DCompile; PSO + root sig are cached on the
// instance.

void GraphicsDX12::EnsurePresentPSO()
{
    if (m_presentPSO) return;

    // Fullscreen triangle using SV_VertexID. UV is top-left = (0,0).
    static const char* kVS =
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut main(uint vid : SV_VertexID)\n"
        "{\n"
        "    VSOut o;\n"
        "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
        "    o.uv  = p;\n"
        "    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n";
    static const char* kPS =
        "Texture2D    g_tex : register(t0);\n"
        "SamplerState g_smp : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "{\n"
        "    return g_tex.Sample(g_smp, uv);\n"
        "}\n";

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> rsBlobOnly;
    ComPtr<ID3DBlob> err;

    DxcCompiler::CompileOptions vsOpt;
    vsOpt.sourceName = "Present.vs";
    vsOpt.entry      = "main";
    vsOpt.stage      = RHI::ShaderStage::VS;
    const auto vsRes = DxcCompiler::Compile(kVS, strlen(kVS), vsOpt);
    if (!vsRes.ok)
    {
        LOG_ERROR("Present VS compile: %s",
                  vsRes.errorMsg.empty() ? "(no diag)" : vsRes.errorMsg.c_str());
        return;
    }

    DxcCompiler::CompileOptions psOpt;
    psOpt.sourceName = "Present.ps";
    psOpt.entry      = "main";
    psOpt.stage      = RHI::ShaderStage::PS;
    const auto psRes = DxcCompiler::Compile(kPS, strlen(kPS), psOpt);
    if (!psRes.ok)
    {
        LOG_ERROR("Present PS compile: %s",
                  psRes.errorMsg.empty() ? "(no diag)" : psRes.errorMsg.c_str());
        return;
    }

    // Root signature: one SRV table + one static linear-clamp sampler.
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(1, params, 1, &samp,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &err)))
    {
        LOG_ERROR("Present root sig: %s",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "(no diag)");
        return;
    }
    ThrowIfFailed(m_device->CreateRootSignature(
        0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_presentRootSig)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = m_presentRootSig.Get();
    psoDesc.VS                    = CD3DX12_SHADER_BYTECODE(vsRes.dxil.data(), vsRes.dxil.size());
    psoDesc.PS                    = CD3DX12_SHADER_BYTECODE(psRes.dxil.data(), psRes.dxil.size());
    psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // matches swap chain (see CreateSwapChain)
    psoDesc.SampleDesc.Count      = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_presentPSO)));
}

void GraphicsDX12::CompositeTextureToSwapChain(uint64_t srvGpuHandle,
                                               RHI::CommandList cmd)
{
    if (srvGpuHandle == 0) return;
    EnsurePresentPSO();
    if (!m_presentPSO) return;

    ID3D12GraphicsCommandList* list = GetPoolEntry(cmd).GetCommandList();

    D3D12_VIEWPORT vp{ 0.f, 0.f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f };
    D3D12_RECT sc{ 0, 0,
        static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);

    list->SetPipelineState(m_presentPSO.Get());
    list->SetGraphicsRootSignature(m_presentRootSig.Get());

    // Ensure the shader-visible heap is bound (caller likely already did, but
    // switching root sigs can reset the implicit binding — re-bind defensively).
    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvUavAllocator.GetGpuHeap() };
    list->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE h{ srvGpuHandle };
    list->SetGraphicsRootDescriptorTable(0, h);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 0, nullptr);
    list->IASetIndexBuffer(nullptr);
    list->DrawInstanced(3, 1, 0, 0);
}

// ===========================================================================
// Window / resize / fullscreen
// ===========================================================================

void GraphicsDX12::ResizeSwapChainAndRtv(UINT width, UINT height)
{
    for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
    ThrowIfFailed(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
        m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < FrameCount; ++n)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
        std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
        m_renderTargets[n]->SetName(bbName.c_str());
        m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    m_width = width; m_height = height;
    m_viewport    = { 0,0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_scissorRect = { 0,0, static_cast<LONG>(width),  static_cast<LONG>(height) };
}

void GraphicsDX12::Resize(uint32_t width, uint32_t height)
{
    if (!width || !height) return;
    WaitForPreviousFrame();
    ResizeSwapChainAndRtv(width, height);
}

void GraphicsDX12::SetFullscreen(bool fullscreen)
{
    if (fullscreen)
    {
        ComPtr<IDXGIOutput> output;
        ThrowIfFailed(m_swapChain->GetContainingOutput(&output));
        DXGI_OUTPUT_DESC od; ThrowIfFailed(output->GetDesc(&od));
        UINT w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
        UINT h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
        WaitForPreviousFrame();
        for (UINT n = 0; n < FrameCount; ++n) m_renderTargets[n].Reset();
        ThrowIfFailed(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
            m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        ThrowIfFailed(m_swapChain->SetFullscreenState(TRUE, output.Get()));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE rh = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT n = 0; n < FrameCount; ++n)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            std::wstring bbName = L"SwapChain.BackBuffer[" + std::to_wstring(n) + L"]";
            m_renderTargets[n]->SetName(bbName.c_str());
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rh);
            rh.ptr += m_rtvDescriptorSize;
        }
        m_width = w; m_height = h;
        m_viewport    = { 0,0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
        m_scissorRect = { 0,0, static_cast<LONG>(w),  static_cast<LONG>(h) };
        CreateHdrRenderTarget(w, h);
    }
    else
    {
        WaitForPreviousFrame();
        ThrowIfFailed(m_swapChain->SetFullscreenState(FALSE, nullptr));
        RECT r; if (GetClientRect(m_hWnd, &r))
        {
            UINT w = r.right - r.left, h = r.bottom - r.top;
            if (w && h) ResizeSwapChainAndRtv(w, h);
        }
    }
}

void GraphicsDX12::SetViewportSize(uint32_t width, uint32_t height)
{
    UINT w = width  ? width  : m_width;
    UINT h = height ? height : m_height;
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w == m_hdrWidth && h == m_hdrHeight) return;
    CreateHdrRenderTarget(w, h);
}

void GraphicsDX12::SetRenderTargetToHdr(const float clearColor[4], RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0,0, static_cast<float>(m_hdrWidth), static_cast<float>(m_hdrHeight), 0,1 };
    D3D12_RECT     sr{ 0,0, static_cast<LONG>(m_hdrWidth),  static_cast<LONG>(m_hdrHeight) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_hdrRtvAllocation.GetCpuHandle();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // Use the same clear value as at creation to avoid CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE
    static const float hdrClear[4] = { 0.f, 0.f, 0.f, 0.f };
    cl->ClearRenderTargetView(rtv, hdrClear, 0, nullptr);
}

void GraphicsDX12::SetRenderTargetToHdrWithDepth(const RHI::Texture* dsv, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(m_hdrWidth), static_cast<float>(m_hdrHeight), 0, 1 };
    D3D12_RECT     sr{ 0, 0, static_cast<LONG>(m_hdrWidth),  static_cast<LONG>(m_hdrHeight) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_hdrRtvAllocation.GetCpuHandle();
    if (dsv && dsv->IsValid() && dsv->handle_id < static_cast<uint32_t>(m_texturePool.size()))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(1, &rtv, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    // No clear — skybox draws on top of existing HDR content.
}

void GraphicsDX12::SetRenderTargetToSwapChain(const float clearColor[4], RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (m_hdrRenderTarget && m_hdrResourceState == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    cl->RSSetViewports(1, &m_viewport);
    cl->RSSetScissorRects(1, &m_scissorRect);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_scRTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

uint64_t GraphicsDX12::GetHdrSceneSrvGpuHandle() const
{
    return m_hdrSrvAllocation.IsValid() ? m_hdrSrvAllocation.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetHdrSceneUavGpuHandle() const
{
    return m_hdrUavAllocation.IsValid() ? m_hdrUavAllocation.GetGpuHandle().ptr : 0ull;
}

void GraphicsDX12::CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !dst.IsValid()) return;
    if (dst.handle_id >= m_texturePool.size()) return;
    ID3D12Resource* dstRes = m_texturePool[dst.handle_id].resource.Get();
    if (!dstRes) return;
    GetPoolEntry(cmd).GetCommandList()->CopyResource(dstRes, m_hdrRenderTarget.Get());
}

uint64_t GraphicsDX12::GetTextureSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureSRVCpuHandleNative(const RHI::Texture& texture) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE nul{};
    if (!texture.IsValid()) return nul;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return nul;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    // GpuCpuHandle = the CPU-visible handle inside the shader-visible heap,
    // suitable as the SOURCE operand for CopyDescriptorsSimple.
    return entry.srv.IsValid() ? entry.srv.GetGpuCpuHandle() : nul;
}

uint64_t GraphicsDX12::GetTextureSRVCpuHandle(const RHI::Texture& texture) const
{
    return GetTextureSRVCpuHandleNative(texture).ptr;
}

void GraphicsDX12::CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                            uint64_t srcCpuHandle,
                                            uint32_t count)
{
    if (dstCpuHandle == 0 || srcCpuHandle == 0 || count == 0) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst{ static_cast<SIZE_T>(dstCpuHandle) };
    D3D12_CPU_DESCRIPTOR_HANDLE src{ static_cast<SIZE_T>(srcCpuHandle) };
    m_device->CopyDescriptorsSimple(count, dst, src,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t GraphicsDX12::GetCbvSrvUavDescriptorIncrement() const
{
    return m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint64_t GraphicsDX12::GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    // Return UNORM alias if available (for SRGB textures), else fall back to normal SRV.
    if (entry.previewSrv.IsValid()) return entry.previewSrv.GetGpuHandle().ptr;
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetTextureStencilSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    return entry.stencilSrv.IsValid() ? entry.stencilSrv.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetTextureUVPlaneSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handle_id >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const Texture_DX12& entry = m_texturePool[texture.handle_id];
    return entry.uvPlaneSrv.IsValid() ? entry.uvPlaneSrv.GetGpuHandle().ptr : 0ull;
}

RHI::IVideoDecoderBackend* GraphicsDX12::GetVideoBackend()
{
    // Fast path — already constructed.
    if (m_videoBackend) return m_videoBackend.get();

    // Lazy init under lock so concurrent first-callers don't double-create.
    std::lock_guard<std::mutex> lock(m_videoBackendMutex);
    if (!m_videoBackend)
    {
        auto backend = std::make_unique<RHI::DX12::VideoDecoderDX12>(*this);
        if (!backend->IsAvailable())
        {
            // Construction logged the failure; expose nullptr so callers can
            // fall back to a no-video code path without further noise.
            return nullptr;
        }
        m_videoBackend = std::move(backend);
    }
    return m_videoBackend.get();
}


// ===========================================================================
// Command recording — state
// ===========================================================================

void GraphicsDX12::BindPipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    if (!pso.IsValid()) return;
    PipelineState_DX12& p = *m_psoPool[pso.handle_id];
    auto& entry = GetPoolEntry(cmd);
    auto* cl    = entry.GetCommandList();
    cl->SetPipelineState(p.pso.Get());
    if (p.isCompute)
    {
        cl->SetComputeRootSignature(p.rootSignature.Get());
        entry.active_rootsig_compute  = p.rootSignature.Get();
    }
    else
    {
        cl->SetGraphicsRootSignature(p.rootSignature.Get());
        entry.active_rootsig_graphics = p.rootSignature.Get();
    }
    entry.active_pso = &pso;
}

void GraphicsDX12::SetPrimitiveTopology(RHI::PrimitiveTopology topology, RHI::CommandList cmd)
{
    auto d3dTopo = ToD3D12Topology(topology);
    GetPoolEntry(cmd).GetCommandList()->IASetPrimitiveTopology(d3dTopo);
    GetPoolEntry(cmd).prev_pt = d3dTopo;
}

void GraphicsDX12::SetViewport(const RHI::Viewport& vp, RHI::CommandList cmd)
{
    D3D12_VIEWPORT d{ vp.top_left_x, vp.top_left_y, vp.width, vp.height, vp.min_depth, vp.max_depth };
    GetPoolEntry(cmd).GetCommandList()->RSSetViewports(1, &d);
}

void GraphicsDX12::SetScissorRect(uint32_t left, uint32_t top,
                                  uint32_t right, uint32_t bottom,
                                  RHI::CommandList cmd)
{
    D3D12_RECT r{ static_cast<LONG>(left), static_cast<LONG>(top),
                  static_cast<LONG>(right), static_cast<LONG>(bottom) };
    GetPoolEntry(cmd).GetCommandList()->RSSetScissorRects(1, &r);
}

// ===========================================================================
// Command recording — resource binding
// ===========================================================================

void GraphicsDX12::BindConstantBuffer(const RHI::GPUBuffer& buffer,
                                      uint32_t slot, RHI::CommandList cmd)
{
    if (!buffer.IsValid() || slot >= kCBVSlotCount) return;
    if (buffer.handle_id >= m_bufferPool.size())
    {
        LOG_ERROR("BindConstantBuffer: handle_id %u out of range (pool size %zu)",
                  buffer.handle_id, m_bufferPool.size());
        return;
    }
    GPUBuffer_DX12& p = m_bufferPool[buffer.handle_id];
    if (!p.resource)
    {
        LOG_ERROR("BindConstantBuffer: buffer handle_id %u has null resource", buffer.handle_id);
        return;
    }
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slot, p.resource->GetGPUVirtualAddress());
}

// Slot-offset variant of BindConstantBuffer — lets ring-buffer owners advance
// through 256-byte aligned slots within a single CB instead of recreating one
// per frame. Used by the reflection probe capture path to upload one CB per
// face inside a 6-slot ring.
void GraphicsDX12::BindConstantBufferAtOffset(uint32_t slot,
                                               const RHI::GPUBuffer& buffer,
                                               uint64_t byteOffset,
                                               RHI::CommandList cmd)
{
    if (!buffer.IsValid() || buffer.handle_id >= m_bufferPool.size()) return;
    GPUBuffer_DX12& p = m_bufferPool[buffer.handle_id];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slot, p.resource->GetGPUVirtualAddress() + byteOffset);
}

// Phase E — bind a raw GPU VA to the per-material custom CBV slot (b8 space0).
// Separate from BindConstantBuffer because the upload-ring owner tracks VAs
// directly, not GPUBuffer handles. Safe to pass 0 as a no-op.
void GraphicsDX12::BindCustomMaterialCBV(uint64_t gpuVA, RHI::CommandList cmd)
{
    if (gpuVA == 0) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        35 /* kCustomMatCBVSlot */, gpuVA);
}

// Phase F — bind a shader-visible GPU handle to the per-material custom
// texture table (t0-t3 space3). Pass a zero handle as a no-op.
void GraphicsDX12::BindCustomMaterialTextureTable(uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(
        36 /* kCustomMatTexSlot */, h);
}

void GraphicsDX12::BindResource(const RHI::GPUResource& resource,
                                uint32_t slot, RHI::CommandList cmd)
{
    if (!resource.IsValid() || slot >= kSRVSlotCount) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();

    if (resource.type == RHI::GPUResource::Type::Texture)
    {
        Texture_DX12& tex = m_texturePool[resource.handle_id];
        if (tex.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slot, tex.srv.GetGpuHandle());
    }
    else if (resource.type == RHI::GPUResource::Type::Buffer)
    {
        GPUBuffer_DX12& buf = m_bufferPool[resource.handle_id];
        if (buf.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slot, buf.srv.GetGpuHandle());
    }
}

void GraphicsDX12::BindSampler(int descriptorIndex, uint32_t slot, RHI::CommandList cmd)
{
    if (descriptorIndex < 0 || static_cast<uint32_t>(descriptorIndex) >= m_samplerPool.size()
        || slot >= kSamplerSlotCount) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(
        kSamplerSlotBase + slot,
        m_samplerPool[static_cast<uint32_t>(descriptorIndex)].GetGpuHandle());
}

// ===========================================================================
// PVF root bindings
// ===========================================================================

void GraphicsDX12::SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                                    RHI::CommandList cmd)
{
    uint32_t values[3] = { v0, v1, v2 };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstants(
        kRootConstantsSlot, 3, values, 0);
}

void GraphicsDX12::SetRootBufferSRV(const RHI::GPUBuffer& buf,
                                    uint32_t rootSlot, RHI::CommandList cmd)
{
    if (!buf.IsValid() || buf.handle_id >= m_bufferPool.size()) return;
    GPUBuffer_DX12& p = m_bufferPool[buf.handle_id];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootShaderResourceView(
        rootSlot, p.resource->GetGPUVirtualAddress());
}

void GraphicsDX12::BindDescriptorTableGpuHandle(uint32_t rootSlot,
                                                uint64_t gpuHandle,
                                                RHI::CommandList cmd)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(rootSlot, h);
}

// ===========================================================================
// Command recording — draws + barriers
// ===========================================================================

void GraphicsDX12::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                                 uint32_t startVertex,  uint32_t startInstance,
                                 RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DrawInstanced(
        vertexCount, instanceCount, startVertex, startInstance);
}

void GraphicsDX12::DrawIndexedInstanced(uint32_t indexCount,  uint32_t instanceCount,
                                        uint32_t startIndex,  int32_t  baseVertex,
                                        uint32_t startInstance, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DrawIndexedInstanced(
        indexCount, instanceCount, startIndex, baseVertex, startInstance);
}

void GraphicsDX12::ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                                        uint64_t argBufferOffset,
                                        uint32_t maxCommands,
                                        const RHI::GPUBuffer* countBuffer,
                                        uint64_t countBufferOffset,
                                        RHI::CommandList cmd)
{
    if (!m_indirectCommandSignature || !argBuffer.IsValid()) return;
    if (argBuffer.handle_id >= m_bufferPool.size()) return;

    ID3D12Resource* argRes = m_bufferPool[argBuffer.handle_id].resource.Get();
    if (!argRes) return;

    ID3D12Resource* countRes = nullptr;
    if (countBuffer && countBuffer->IsValid() && countBuffer->handle_id < m_bufferPool.size())
        countRes = m_bufferPool[countBuffer->handle_id].resource.Get();

    GetPoolEntry(cmd).GetCommandList()->ExecuteIndirect(
        m_indirectCommandSignature.Get(),
        maxCommands,
        argRes,
        argBufferOffset,
        countRes,
        countRes ? countBufferOffset : 0);
}

void GraphicsDX12::CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                               uint64_t size, RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handle_id >= m_bufferPool.size() || dst.handle_id >= m_bufferPool.size()) return;
    ID3D12Resource* srcRes = m_bufferPool[src.handle_id].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handle_id].resource.Get();
    if (!srcRes || !dstRes || size == 0) return;
    GetPoolEntry(cmd).GetCommandList()->CopyBufferRegion(dstRes, 0, srcRes, 0, size);
}

void GraphicsDX12::CopyTextureSubresource(const RHI::Texture& src,
                                           uint32_t srcMip, uint32_t srcArraySlice,
                                           const RHI::Texture& dst,
                                           uint32_t dstMip, uint32_t dstArraySlice,
                                           RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handle_id >= m_texturePool.size() || dst.handle_id >= m_texturePool.size()) return;

    const Texture_DX12& srcEntry = m_texturePool[src.handle_id];
    const Texture_DX12& dstEntry = m_texturePool[dst.handle_id];
    ID3D12Resource* srcRes = srcEntry.resource.Get();
    ID3D12Resource* dstRes = dstEntry.resource.Get();
    if (!srcRes || !dstRes) return;

    const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dstRes->GetDesc();

    // D3D12CalcSubresource(mip, arraySlice, planeSlice, mipLevels, arraySize)
    const UINT srcSub = D3D12CalcSubresource(
        srcMip, srcArraySlice, 0, srcDesc.MipLevels, srcDesc.DepthOrArraySize);
    const UINT dstSub = D3D12CalcSubresource(
        dstMip, dstArraySlice, 0, dstDesc.MipLevels, dstDesc.DepthOrArraySize);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcRes;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = srcSub;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = dstRes;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = dstSub;

    GetPoolEntry(cmd).GetCommandList()->CopyTextureRegion(
        &dstLoc, 0, 0, 0, &srcLoc, /*pSrcBox*/ nullptr);
}

// ===========================================================================
// Compute dispatch
// ===========================================================================

void GraphicsDX12::BindComputePipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    BindPipelineState(pso, cmd);
}

void GraphicsDX12::DispatchCompute(uint32_t x, uint32_t y, uint32_t z, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->Dispatch(x, y, z);
}

void GraphicsDX12::DispatchMesh(uint32_t x, uint32_t y, uint32_t z, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->DispatchMesh(x, y, z);
}

void GraphicsDX12::SetComputeRootCBV(uint32_t rootSlot, const RHI::GPUBuffer& cb, uint32_t byteOffset, RHI::CommandList cmd)
{
    if (!cb.IsValid()) return;
    const GPUBuffer_DX12& p = m_bufferPool[cb.handle_id];
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootConstantBufferView(
        rootSlot, p.resource->GetGPUVirtualAddress() + byteOffset);
}

void GraphicsDX12::SetComputeDescriptorTable(uint32_t rootSlot, uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootDescriptorTable(rootSlot, h);
}

uint64_t GraphicsDX12::GetTextureUAVGpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid()) return 0;
    const Texture_DX12& t = m_texturePool[tex.handle_id];
    if (!t.uav.IsValid()) return 0;
    return t.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBuffer_DX12& b = m_bufferPool[buf.handle_id];
    if (!b.uav.IsValid()) return 0;
    return b.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBuffer_DX12& b = m_bufferPool[buf.handle_id];
    if (!b.srv.IsValid()) return 0;
    return b.srv.GetGpuHandle().ptr;
}

void GraphicsDX12::SetHdrTextureState(RHI::ResourceState newState, RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget) return;
    const D3D12_RESOURCE_STATES target = ToD3D12ResourceState(newState);
    if (m_hdrResourceState == target) return;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_hdrRenderTarget.Get(), m_hdrResourceState, target);
    GetPoolEntry(cmd).GetCommandList()->ResourceBarrier(1, &barrier);
    m_hdrResourceState = target;
}

void GraphicsDX12::PushBarrier(const RHI::GPUBarrier& barrier, RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    // Helper: resolve a generic GPUResource* → ID3D12Resource*
    auto resolveResource = [this](const RHI::GPUResource* res) -> ID3D12Resource*
    {
        if (!res || !res->IsValid()) return nullptr;
        if (res->type == RHI::GPUResource::Type::Texture)
            return m_texturePool[res->handle_id].resource.Get();
        if (res->type == RHI::GPUResource::Type::Buffer)
            return m_bufferPool[res->handle_id].resource.Get();
        return nullptr;
    };

    switch (barrier.type)
    {
    case RHI::GPUBarrier::Type::MEMORY:
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::UAV(
            resolveResource(barrier.memory.resource));
        cl->ResourceBarrier(1, &b);
        break;
    }
    case RHI::GPUBarrier::Type::IMAGE:
    {
        Texture_DX12& t = m_texturePool[barrier.image.texture->handle_id];
        D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.image.layout_before);
        D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.image.layout_after);
        UINT subresource = (barrier.image.mip >= 0 && barrier.image.slice >= 0)
            ? D3D12CalcSubresource(barrier.image.mip, barrier.image.slice,
                                   0, barrier.image.texture->desc.mip_levels,
                                   barrier.image.texture->desc.array_size)
            : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after, subresource);
        cl->ResourceBarrier(1, &b);
        t.state = after;
        break;
    }
    case RHI::GPUBarrier::Type::BUFFER:
    {
        GPUBuffer_DX12& buf = m_bufferPool[barrier.buffer.buffer->handle_id];
        D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.buffer.state_before);
        D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.buffer.state_after);
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(buf.resource.Get(), before, after);
        cl->ResourceBarrier(1, &b);
        buf.state = after;
        break;
    }
    case RHI::GPUBarrier::Type::ALIASING:
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Aliasing(
            resolveResource(barrier.aliasing.resource_before),
            resolveResource(barrier.aliasing.resource_after));
        cl->ResourceBarrier(1, &b);
        break;
    }
    }
}

// ===========================================================================
// New IGraphicsDevice methods — render targets, heap binding, buffer mapping,
// resource destruction
// ===========================================================================

void GraphicsDX12::SetRenderTargets(uint32_t numRTs,
                                     const RHI::Texture* const* rtvs,
                                     const RHI::Texture*        dsv,
                                     RHI::CommandList           cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    for (uint32_t i = 0; i < numRTs && i < 8; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[rtvs[i]->handle_id].rtv.GetCpuHandle();
    }

    if (dsv && dsv->IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::SetRenderTargetsAndHdr(uint32_t numRTs,
                                           const RHI::Texture* const* rtvs,
                                           const RHI::Texture*        dsv,
                                           RHI::CommandList           cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;

    // Transition HDR to RENDER_TARGET if needed (mirrors SetRenderTargetToHdr).
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Build RTV array: caller-supplied RTs first, HDR appended last. Cap at
    // D3D12's 8-RT limit (numRTs+1 must be ≤ 8).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    const uint32_t capped = (numRTs > 7) ? 7u : numRTs;
    for (uint32_t i = 0; i < capped; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[rtvs[i]->handle_id].rtv.GetCpuHandle();
    }
    rtvHandles[capped] = m_hdrRtvAllocation.GetCpuHandle();
    const uint32_t totalRTs = capped + 1;

    if (dsv && dsv->IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_texturePool[dsv->handle_id].dsv.GetCpuHandle();
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::ClearRenderTarget(const RHI::Texture& texture,
                                      const float         color[4],
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearRenderTargetView(
        m_texturePool[texture.handle_id].rtv.GetCpuHandle(), color, 0, nullptr);
}

void GraphicsDX12::ClearHdrRenderTarget(const float      color[4],
                                         RHI::CommandList cmd)
{
    if (!m_hdrRenderTarget || !m_hdrRtvAllocation.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    // ClearRenderTargetView requires the resource in RENDER_TARGET state.
    if (m_hdrResourceState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRenderTarget.Get(), m_hdrResourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        m_hdrResourceState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Match the texture's clear-value contract (set at creation: 0,0,0,0).
    // Pass user-supplied colour through; D3D validation will warn if it
    // diverges from the optimised clear value but won't fail.
    cl->ClearRenderTargetView(m_hdrRtvAllocation.GetCpuHandle(), color, 0, nullptr);
}

void GraphicsDX12::ClearDepthStencil(const RHI::Texture& texture,
                                      float               depth,
                                      uint8_t             stencil,
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    auto& tex = m_texturePool[texture.handle_id];

    // Also clear the STENCIL plane when the format carries one. The deferred
    // pipeline stencil-gates LightingPass on per-pixel shading-model ids that
    // GBuffer writes via REPLACE; if stencil is never cleared, pixels NOT
    // rasterized this frame keep stale ids and get re-shaded next frame. That's
    // invisible when geometry+sky cover every pixel, but Wireframe view (sparse
    // edge coverage + suppressed sky) makes the stale ids accumulate as a
    // "never cleared" smear. Gate on a stencil-bearing format so depth-only
    // targets (shadow maps, D32) don't trip the debug layer.
    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
    if (tex.resource)
    {
        const DXGI_FORMAT f = tex.resource->GetDesc().Format;
        if (f == DXGI_FORMAT_D24_UNORM_S8_UINT  || f == DXGI_FORMAT_R24G8_TYPELESS ||
            f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS)
            flags |= D3D12_CLEAR_FLAG_STENCIL;
    }
    cl->ClearDepthStencilView(
        tex.dsv.GetCpuHandle(),
        flags, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetDepthStencilSlice(const RHI::Texture& depthTex,
                                         uint32_t            arraySlice,
                                         RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handle_id];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = tex.sliceDsvs[arraySlice].GetCpuHandle();
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
}

void GraphicsDX12::ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                           uint32_t            arraySlice,
                                           float               depth,
                                           uint8_t             stencil,
                                           RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handle_id];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearDepthStencilView(tex.sliceDsvs[arraySlice].GetCpuHandle(),
                              D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetStencilRef(uint32_t ref, RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->OMSetStencilRef(ref);
}

void GraphicsDX12::SetGraphicsRootConstant(uint32_t rootSlot,
                                            uint32_t value,
                                            uint32_t offsetInWords,
                                            RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->SetGraphicsRoot32BitConstant(rootSlot, value, offsetInWords);
}

uint64_t GraphicsDX12::GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                                  uint32_t mip) const
{
    if (!tex.IsValid()) return 0;
    const auto& entry = m_texturePool[tex.handle_id];
    if (mip >= entry.mipUavs.size()) return 0;
    if (!entry.mipUavs[mip].IsValid()) return 0;
    return entry.mipUavs[mip].GetGpuHandle().ptr;
}

// ---- Lazy per-cube descriptor views (reflection probe capture / prefilter) --
// These methods mutate the texture entry on first request to allocate a CPU
// descriptor in the appropriate heap. Returning a handle is constant-time on
// subsequent calls.
uint64_t GraphicsDX12::GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                                       uint32_t cubeIdx,
                                                       uint32_t face,
                                                       uint32_t mip)
{
    if (!tex.IsValid() || face >= 6) return 0;
    auto& entry = m_texturePool[tex.handle_id];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 16) | (face << 8) | mip;
    auto it = entry.cubeFaceRtvs.find(key);
    if (it != entry.cubeFaceRtvs.end())
        return it->second.GetCpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t arraySlice = cubeIdx * 6u + face;
    if (arraySlice >= rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_rtvAllocator.Allocate(1);
    if (!alloc.IsValid()) return 0;

    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = rd.Format;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvd.Texture2DArray.MipSlice        = mip;
    rtvd.Texture2DArray.FirstArraySlice = arraySlice;
    rtvd.Texture2DArray.ArraySize       = 1;
    rtvd.Texture2DArray.PlaneSlice      = 0;
    m_device->CreateRenderTargetView(entry.resource.Get(), &rtvd, alloc.GetCpuHandle());

    auto [emplaceIt, _ok] = entry.cubeFaceRtvs.emplace(key, alloc);
    return emplaceIt->second.GetCpuHandle().ptr;
}

uint64_t GraphicsDX12::GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                                      uint32_t cubeIdx,
                                                      uint32_t mip)
{
    if (!tex.IsValid()) return 0;
    auto& entry = m_texturePool[tex.handle_id];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 8) | mip;
    auto it = entry.cubeMipUavs.find(key);
    if (it != entry.cubeMipUavs.end())
        return it->second.GetGpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t firstSlice = cubeIdx * 6u;
    if (firstSlice + 6u > rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(1);
    if (!alloc.IsValid()) return 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = rd.Format;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uavd.Texture2DArray.MipSlice        = mip;
    uavd.Texture2DArray.FirstArraySlice = firstSlice;
    uavd.Texture2DArray.ArraySize       = 6;
    uavd.Texture2DArray.PlaneSlice      = 0;
    m_device->CreateUnorderedAccessView(entry.resource.Get(), nullptr, &uavd, alloc.GetCpuHandle());
    alloc.CopyToGpu();

    auto [emplaceIt, _ok] = entry.cubeMipUavs.emplace(key, alloc);
    return emplaceIt->second.GetGpuHandle().ptr;
}


uint32_t GraphicsDX12::BeginGPUTimestamp(RHI::CommandList cmd, const char* name)
{
    if (!m_gpuProfiler.enabled) return ~0u;
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return ~0u;
    // CL pool entry already tracks which queue it was opened on; just forward
    // it so the profiler can split totals + compute the critical path.
    return m_gpuProfiler.BeginTimestamp(cl, name,
                                        static_cast<uint8_t>(entry.queue));
}

void GraphicsDX12::EndGPUTimestamp(RHI::CommandList cmd, uint32_t regionIndex)
{
    if (regionIndex == ~0u) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    m_gpuProfiler.EndTimestamp(cl, regionIndex);
}

void GraphicsDX12::BindDescriptorHeaps(RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    ID3D12DescriptorHeap* heaps[] =
    {
        m_cbvSrvUavAllocator.GetHeap(),
        m_samplerAllocator.GetHeap()
    };
    cl->SetDescriptorHeaps(2, heaps);
}

void* GraphicsDX12::MapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return nullptr;
    void* ptr = nullptr;
    D3D12_RANGE rr{ 0, 0 };
    m_bufferPool[buffer.handle_id].resource->Map(0, &rr, &ptr);
    return ptr;
}

void GraphicsDX12::UnmapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return;
    m_bufferPool[buffer.handle_id].resource->Unmap(0, nullptr);
}

void GraphicsDX12::CopyTexturePixelToBuffer(const RHI::Texture& src,
                                             uint32_t srcX, uint32_t srcY,
                                             RHI::GPUBuffer& dst,
                                             RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    Texture_DX12&  srcEntry = m_texturePool[src.handle_id];
    GPUBuffer_DX12& dstEntry = m_bufferPool[dst.handle_id];

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcEntry.resource.Get();
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource                          = dstEntry.resource.Get();
    dstLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset             = 0;
    dstLoc.PlacedFootprint.Footprint.Format   = ToDxgiFormat(src.desc.format);
    dstLoc.PlacedFootprint.Footprint.Width    = 1;
    dstLoc.PlacedFootprint.Footprint.Height   = 1;
    dstLoc.PlacedFootprint.Footprint.Depth    = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    const D3D12_BOX srcBox = { srcX, srcY, 0u, srcX + 1u, srcY + 1u, 1u };
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
}


void GraphicsDX12::DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource)
{
    if (!resource) return;
    // Same deferral discipline as Destroy{Buffer,Texture}: hand the resource to
    // the current backbuffer's release slot so it survives until BeginFrame has
    // waited out the GPU. No pool slot to recycle — this is a non-pool resource.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    m_deferredRelease[m_frameIndex].resources.push_back(std::move(resource));
}

void GraphicsDX12::DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource,
                                        uint32_t queueIndex,
                                        uint64_t fenceValue)
{
    if (!resource) return;
    if (queueIndex >= 3)
    {
        // Bad queue index — fall back to slot-based defer so resource still gets freed.
        DeferReleaseResource(std::move(resource));
        return;
    }
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    m_fenceKeyedReleases.push_back({ queueIndex, fenceValue, std::move(resource) });
}

uint64_t GraphicsDX12::GetLastSignaledFenceValue(uint32_t queueIndex) const
{
    if (queueIndex >= 3) return 0;
    return m_queueFenceValues[queueIndex];
}

void GraphicsDX12::ProcessFenceKeyedReleases()
{
    std::deque<FenceKeyedRelease> drained;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        // Walk the queue; move entries whose fence has completed into `drained`
        // so the ComPtr destruction happens OUTSIDE the lock (Release() can
        // re-enter graphics-device calls in theory; defensive).
        for (auto it = m_fenceKeyedReleases.begin(); it != m_fenceKeyedReleases.end(); )
        {
            const uint32_t qi = it->queueIndex;
            ID3D12Fence* fence = (qi == 0) ? m_fence.Get() : m_queueFences[qi].Get();
            if (!fence)
            {
                // Fence not initialized — safest to keep the entry (drop later).
                ++it;
                continue;
            }
            if (fence->GetCompletedValue() >= it->fenceValue)
            {
                drained.push_back(std::move(*it));
                it = m_fenceKeyedReleases.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // `drained` destructs here; ComPtrs release ID3D12Resources outside the lock.
}

// ===========================================================================
// PSO Library — disk-based ISA cache
// ===========================================================================

bool GraphicsDX12::InitPSOLibrary(const char* cacheFilePath)
{
    // Try to get ID3D12Device1 (required for CreatePipelineLibrary)
    ComPtr<ID3D12Device1> device1;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        LOG_WARNING("InitPSOLibrary: ID3D12Device1 not available — PSO library disabled");
        return false;
    }

    // Read existing cache from disk (optional — graceful if missing)
    const void* pData    = nullptr;
    SIZE_T      dataSize = 0;

    if (cacheFilePath && cacheFilePath[0] != '\0')
    {
        std::ifstream f(cacheFilePath, std::ios::binary | std::ios::ate);
        if (f.is_open())
        {
            const auto sz = f.tellg();
            f.seekg(0, std::ios::beg);
            m_psoLibraryData.resize(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(m_psoLibraryData.data()), sz);
            if (f.good())
            {
                pData    = m_psoLibraryData.data();
                dataSize = m_psoLibraryData.size();
            }
            else
            {
                m_psoLibraryData.clear();
            }
        }
    }

    HRESULT hr = device1->CreatePipelineLibrary(pData, dataSize, IID_PPV_ARGS(&m_psoLibrary));
    if (FAILED(hr))
    {
        // D3D12_ERROR_DRIVER_VERSION_MISMATCH or D3D12_ERROR_ADAPTER_NOT_FOUND
        // means the cached data is stale — start fresh.
        LOG_WARNING("InitPSOLibrary: CreatePipelineLibrary failed (0x%08X), starting empty library",
                    static_cast<unsigned>(hr));
        m_psoLibraryData.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_psoLibrary));
        if (FAILED(hr))
        {
            LOG_WARNING("InitPSOLibrary: could not create empty library (0x%08X) — PSO library disabled",
                        static_cast<unsigned>(hr));
            m_psoLibrary = nullptr;
            return false;
        }
    }

    LOG_INFO("InitPSOLibrary: PSO library ready (%zu bytes from disk)", dataSize);
    return true;
}

void GraphicsDX12::SavePSOLibrary(const char* cacheFilePath)
{
    if (!m_psoLibrary || !cacheFilePath || cacheFilePath[0] == '\0')
        return;

    const SIZE_T serializedSize = m_psoLibrary->GetSerializedSize();
    if (serializedSize == 0)
        return;

    std::vector<uint8_t> buf(serializedSize);
    HRESULT hr = m_psoLibrary->Serialize(buf.data(), serializedSize);
    if (FAILED(hr))
    {
        LOG_ERROR("SavePSOLibrary: Serialize failed (0x%08X)", static_cast<unsigned>(hr));
        return;
    }

    std::ofstream f(cacheFilePath, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
    {
        LOG_ERROR("SavePSOLibrary: cannot open '%s' for writing", cacheFilePath);
        return;
    }
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    if (f.good())
        LOG_INFO("SavePSOLibrary: saved %zu bytes to '%s'", serializedSize, cacheFilePath);
    else
        LOG_ERROR("SavePSOLibrary: write error for '%s'", cacheFilePath);
}

