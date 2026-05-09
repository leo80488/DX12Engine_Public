#pragma once

// DxcCompiler — thin wrapper around IDxcCompiler3 + IDxcUtils.
//
// One central place to compile HLSL → DXIL container and to build an
// ID3D12ShaderReflection from a container blob. ShaderLibrary, ShaderImporter,
// and a couple of inline-shader call sites all funnel through here so the
// entire engine speaks one DXC dialect (HV 2018, SM 6.6) and we don't end up
// with two parallel compile paths to keep in sync.

#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12ShaderReflection;

namespace DxcCompiler
{
    struct Define
    {
        std::string name;
        std::string value;   // "1" / "0" — empty means -D name (no value)
    };

    struct CompileOptions
    {
        std::string         sourceName;          // diagnostic display + virtual path for relative includes
        std::string         entry = "main";
        RHI::ShaderStage    stage = RHI::ShaderStage::PS;
        std::string         includeDir;          // extra -I directory; empty to skip
        std::vector<Define> defines;
        bool                debug = false;       // -Zi -Od when true; -O3 otherwise
    };

    struct CompileResult
    {
        bool                 ok = false;
        std::vector<uint8_t> dxil;       // full DXIL container (matches IDxcResult::GetOutput(DXC_OUT_OBJECT))
        std::string          errorMsg;   // diagnostic text — populated on warnings AND failures
    };

    // Compile HLSL source to a DXIL container. Thread-safe.
    CompileResult Compile(const void* src, std::size_t srcSize, const CompileOptions& opts);

    // Build an ID3D12ShaderReflection over a DXIL container (the same blob
    // returned in CompileResult::dxil). Returns nullptr if reflection cannot
    // be built — caller owns the returned COM pointer and must Release().
    ID3D12ShaderReflection* CreateReflection(const void* container, std::size_t containerSize);

    // SM 6.6 profile string for a stage. "vs_6_6", "ps_6_6", "cs_6_6", ...
    const char*    ProfileForStage(RHI::ShaderStage stage);
    const wchar_t* ProfileForStageW(RHI::ShaderStage stage);
}
