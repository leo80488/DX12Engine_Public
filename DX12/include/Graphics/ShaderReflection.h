#pragma once

// ShaderReflection — wraps DXC's IDxcUtils::CreateReflection so the rest of
// the engine can ask "what does this compiled shader want bound?" without
// depending on <d3d12shader.h> or <dxcapi.h>. Output structs are POD-ish,
// copyable, and contain no COM types — safe to cache, serialize, or pass
// across module boundaries.
//
// Used by:
//   - Phase A: smoke-tested on its own (validates the API against an .ishdr blob)
//   - Phase B+: ShaderLibrary stores Reflection alongside each compiled
//     shader so MaterialEditor / GBufferPass can drive bindings + UI from
//     the shader's actual signature.
//
// Reflection covers what we need for material UI / binding generation:
//   * resource bindings (SRV textures, samplers, CBVs, structured buffers)
//   * constant buffer layouts (variable name / offset / size / type)
//
// Out of scope (add when needed): input-layout signatures, output
// signatures, thread-group sizes for compute, function-link information.

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderReflect
{

enum class ResourceType : uint8_t
{
    Texture,            // SRV: Texture1D/2D/3D/Cube
    Sampler,
    CBuffer,
    StructuredBuffer,   // SRV: StructuredBuffer<T>
    RWTexture,          // UAV
    RWStructuredBuffer, // UAV
    ByteAddressBuffer,
    Other
};

enum class TextureDim : uint8_t
{
    Unknown,
    Tex1D, Tex1DArray,
    Tex2D, Tex2DArray, Tex2DMS, Tex2DMSArray,
    Tex3D,
    TexCube, TexCubeArray,
    Buffer
};

enum class VarType : uint8_t
{
    Unknown,
    Bool,
    Float, Float2, Float3, Float4,
    Float4x4,
    Int,   Int2,   Int3,   Int4,
    UInt,  UInt2,  UInt3,  UInt4,
    Struct
};

struct ResourceBinding
{
    std::string  name;
    ResourceType type       = ResourceType::Other;
    TextureDim   dim        = TextureDim::Unknown;
    uint32_t     bindPoint  = 0;     // register slot (e.g. t0 → 0)
    uint32_t     bindCount  = 1;     // 0 = unbounded array (Texture2D arr[])
    uint32_t     space      = 0;     // register space
};

struct CBufferVar
{
    std::string name;
    VarType     type     = VarType::Unknown;
    uint32_t    offset   = 0;        // bytes from start of cbuffer
    uint32_t    size     = 0;        // bytes (single element for arrays)
    uint32_t    elements = 0;        // 0 = scalar/vector, N = array length
};

struct CBufferLayout
{
    std::string             name;
    uint32_t                bindPoint = 0;
    uint32_t                space     = 0;
    uint32_t                sizeBytes = 0;
    std::vector<CBufferVar> vars;
};

struct Reflection
{
    std::vector<ResourceBinding> bindings;   // textures + samplers + cbuffers + buffers
    std::vector<CBufferLayout>   cbuffers;   // detailed layouts for each CBV
};

// Reflect compiled DXIL container bytecode into Reflection. Returns false
// (and logs) if DXC rejects the bytecode (corrupt blob, no reflection part).
//
// The first parameter is named `dxbc` for historical reasons — the bytecode
// is now a DXIL container, but the surrounding container format and the
// caller-side semantics are unchanged.
//
// Safe to call from any thread — IDxcUtils is thread-safe.
bool Reflect(const void* dxbc, std::size_t dxbcSize, Reflection& out);

// Human-readable names for logging / inspector display.
const char* ToString(ResourceType t);
const char* ToString(VarType t);
const char* ToString(TextureDim d);

} // namespace ShaderReflect
