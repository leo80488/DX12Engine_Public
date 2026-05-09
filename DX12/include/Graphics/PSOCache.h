#pragma once

// PSOCache — in-memory pipeline state object deduplication.
//
// Responsibility boundary:
//   - Caches RHI::PipelineState objects keyed by a hash of PSODesc.
//   - On cache miss: retrieves RHI::Shader handles from ShaderLibrary,
//     assembles RHI::PipelineStateDesc, calls IGraphicsDevice::CreatePipelineState.
//   - ID3D12PipelineLibrary disk ISA caching is handled transparently inside
//     IGraphicsDevice::CreatePipelineState / InitPSOLibrary / SavePSOLibrary.
//   - Thread-safe: GetOrCreate is guarded by an internal mutex.
//
// Usage contract:
//   - Call Init() once at startup.
//   - Call GetOrCreate() at load time for warm-up; also safe at runtime for
//     unanticipated permutations (returns fallback PSO immediately, async compile queued).
//   - No DX12 types appear in this header.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/PermutationKey.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <array>

class IGraphicsDevice;
class ShaderLibrary;
enum class ShaderID : uint32_t;

// Input layout type — selects the vertex attribute layout baked into the PSO.
// Four canonical layouts; minimising layout count directly reduces PSO count.
enum class InputLayoutType : uint8_t
{
    None        = 0,  // Null layout (fullscreen triangle / billboard — SV_VertexID only)
    StaticMesh  = 1,  // POSITION(f3) NORMAL(f3) COLOR(f3)
    // SkinnedMesh / Billboard — add as needed
};

// PSODesc — plain-data descriptor used as the PSOCache key.
// All fields must be value-comparable and hashable.
struct PSODesc
{
    ShaderID         vsID       = static_cast<ShaderID>(~0u);
    ShaderID         psID       = static_cast<ShaderID>(~0u);
    PermutationKey   perm       = {};

    InputLayoutType  inputLayout = InputLayoutType::None;

    // Render state (copied by value for hashing)
    RHI::RasterizerState   rs  = {};
    RHI::DepthStencilState dss = {};
    RHI::BlendState        bs  = {};

    RHI::PrimitiveTopology     topology   = RHI::PrimitiveTopology::TRIANGLELIST;

    std::array<RHI::Format, 8> rtvFormats = {};
    uint32_t                   rtvCount   = 0;
    RHI::Format                dsvFormat  = RHI::Format::UNKNOWN;
    uint32_t                   sampleCount = 1;

    // Stable hash over all fields.
    uint64_t Hash() const noexcept;
};

class PSOCache
{
public:
    void Init(IGraphicsDevice& gfx, ShaderLibrary& shaderLib);
    void Shutdown();

    // Returns a pointer into the internal cache table (stable for lifetime of PSOCache).
    // Thread-safe.  Creates the PSO on first call for a unique desc.
    // Returns nullptr if shader lookup or PSO creation fails.
    const RHI::PipelineState* GetOrCreate(const PSODesc& desc);

    // Drop every cached PSO. The next GetOrCreate call rebuilds via the
    // backend (which goes through ID3D12PipelineLibrary if cached on disk).
    // Hot-reload calls this after ShaderLibrary::ClearCaches so subsequent
    // GetOrCreate forces a fresh DXC compile path.
    //
    // Pointers returned by previous GetOrCreate calls become DANGLING. Callers
    // must re-fetch — typically by re-running the owning pass's Init.
    void Clear();

    // Build an InputLayout for a given InputLayoutType (shared helper).
    static RHI::InputLayout BuildInputLayout(InputLayoutType type);

private:
    IGraphicsDevice* m_gfx       = nullptr;
    ShaderLibrary*   m_shaderLib = nullptr;

    std::unordered_map<uint64_t, RHI::PipelineState> m_cache;
    // Negative cache: PSODesc.Hash() values whose CreateNew failed. Without
    // this, every per-frame draw against a material with a broken custom PS
    // re-runs CreateNew → GetShader → logs ERROR — same spam pattern as the
    // ShaderLibrary one we suppressed. Cleared by Clear(), which fires on
    // hot-reload, so a fixed-up shader gets retried automatically.
    std::unordered_set<uint64_t> m_failedPSOs;
    std::mutex m_mutex;

    // Creates a new PSO; called on cache miss.
    // Returns an invalid RHI::PipelineState on error.
    RHI::PipelineState CreateNew(const PSODesc& desc);
};
