#pragma once

#include "Resource/IImporter.h"

namespace Resource
{
    // Compiles .hlsl source to the internal .ishdr format.
    //
    // .ishdr blob layout:
    //   [AssetHeader (20 B)] [ShaderMetadata] [DXBC bytecode]
    //
    // D3DCompile runs here (import time).  ShaderLoader only reads the DXBC
    // bytecode from an .ishdr blob — no compilation at load time.
    //
    // Stage inference follows the filename suffix convention:
    //   .vs.hlsl → VS,  .ps.hlsl → PS,  .cs.hlsl → CS,
    //   .gs.hlsl → GS,  .hs.hlsl → HS,  .ds.hlsl → DS
    class ShaderImporter : public IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override { return { ".hlsl" }; }
        const char* GetInternalExtension() const override { return ".ishdr"; }
    };
}
