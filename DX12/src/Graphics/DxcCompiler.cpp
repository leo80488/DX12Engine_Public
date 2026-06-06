#include "Graphics/DxcCompiler.h"

#include "System/Log.h"
#include "Resource/AssetFS.h"   // AssetFS-backed #include resolution for packed builds

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>

#include <dxcapi.h>
#include <d3d12shader.h>

#include <filesystem>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace
{
    // Lazy-init globals. IDxcCompiler3 / IDxcUtils are documented thread-safe;
    // creating them is not free, so we share one pair across the process.
    std::once_flag       g_initFlag;
    ComPtr<IDxcUtils>    g_utils;
    ComPtr<IDxcCompiler3> g_compiler;
    bool                 g_initOk = false;

    void EnsureInit()
    {
        std::call_once(g_initFlag, []
        {
            HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils));
            if (FAILED(hr))
            {
                LOG_ERROR("DxcCompiler: DxcCreateInstance(DxcUtils) failed (hr=0x%08X)", hr);
                return;
            }
            hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler));
            if (FAILED(hr))
            {
                LOG_ERROR("DxcCompiler: DxcCreateInstance(DxcCompiler) failed (hr=0x%08X)", hr);
                return;
            }
            g_initOk = true;
        });
    }

    // UTF-8 → UTF-16. DXC takes wide-character argument arrays.
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                             static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                            static_cast<int>(s.size()), w.data(), wlen);
        return w;
    }

    // UTF-16 → UTF-8 (DXC hands include paths back as wide strings).
    std::string Narrow(const wchar_t* w)
    {
        if (!w || !*w) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string s(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
        return s;
    }

    std::string NormalizeKey(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        while (s.rfind("./", 0) == 0) s.erase(0, 2);
        return s;
    }

    // IDxcIncludeHandler that resolves #include "X.hlsli" through AssetFS
    // (game.ipak first, loose-disk fallback). The stock CreateDefaultIncludeHandler
    // reads the real filesystem ONLY, so in a PACKED build every runtime DXC compile
    // that #includes a shared header failed — all DDGI compute shaders pull
    // DDGICommon.hlsli / DDGISampling.hlsli / cluster_common.hlsli, which live
    // inside game.ipak, so DDGI was silently dead in shipped builds. dirA/dirB hold
    // the source-file parent + explicit -I dir so a bare basename still resolves.
    class AssetFSIncludeHandler : public IDxcIncludeHandler
    {
    public:
        std::string dirA;   // source-file parent (normalised, no trailing slash)
        std::string dirB;   // explicit includeDir (normalised, no trailing slash)

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename,
                                             IDxcBlob** ppIncludeSource) override
        {
            if (ppIncludeSource) *ppIncludeSource = nullptr;
            if (!pFilename || !g_utils) return E_FAIL;

            const std::string norm = NormalizeKey(Narrow(pFilename));
            std::string base = norm;
            if (auto s = norm.find_last_of('/'); s != std::string::npos)
                base = norm.substr(s + 1);

            std::string cands[4];
            int n = 0;
            cands[n++] = norm;                                   // as DXC joined it
            if (!dirA.empty()) cands[n++] = dirA + "/" + base;   // source-relative
            if (!dirB.empty()) cands[n++] = dirB + "/" + base;   // -I relative
            cands[n++] = base;                                   // last resort

            std::vector<std::uint8_t> bytes;
            for (int i = 0; i < n; ++i)
            {
                if (!Resource::AssetFS::Get().ReadFile(cands[i], bytes) || bytes.empty())
                    continue;
                ComPtr<IDxcBlobEncoding> blob;
                if (FAILED(g_utils->CreateBlob(bytes.data(),
                                               static_cast<UINT32>(bytes.size()),
                                               DXC_CP_UTF8, &blob)))
                    return E_FAIL;
                *ppIncludeSource = blob.Detach();
                return S_OK;
            }
            return E_FAIL;   // not found → DXC emits the include error
        }

        // Stack-scoped per Compile() call; no real refcounting needed.
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv && (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)))
            { *ppv = this; return S_OK; }
            if (ppv) *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef()  override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
    };
} // namespace

namespace DxcCompiler
{

const char* ProfileForStage(RHI::ShaderStage stage)
{
    // SM 6.6 picks up wave intrinsics + DescriptorHeap[] dynamic indexing —
    // the things that make a bindless renderer pleasant to write. Bumping
    // higher (6.7) buys little for our pipeline today.
    switch (stage)
    {
    case RHI::ShaderStage::VS: return "vs_6_6";
    case RHI::ShaderStage::PS: return "ps_6_6";
    case RHI::ShaderStage::CS: return "cs_6_6";
    case RHI::ShaderStage::GS: return "gs_6_6";
    case RHI::ShaderStage::HS: return "hs_6_6";
    case RHI::ShaderStage::DS: return "ds_6_6";
    case RHI::ShaderStage::MS: return "ms_6_6";
    case RHI::ShaderStage::AS: return "as_6_6";
    // DXR ray-tracing libraries: lib_6_3 minimum, lib_6_5 enables some
    // raytracing features (e.g. inline RT). Stick to 6_5 to align with the
    // engine's broader SM target.
    case RHI::ShaderStage::LIB: return "lib_6_5";
    default:                   return "ps_6_6";
    }
}

const wchar_t* ProfileForStageW(RHI::ShaderStage stage)
{
    switch (stage)
    {
    case RHI::ShaderStage::VS: return L"vs_6_6";
    case RHI::ShaderStage::PS: return L"ps_6_6";
    case RHI::ShaderStage::CS: return L"cs_6_6";
    case RHI::ShaderStage::GS: return L"gs_6_6";
    case RHI::ShaderStage::HS: return L"hs_6_6";
    case RHI::ShaderStage::DS: return L"ds_6_6";
    case RHI::ShaderStage::MS: return L"ms_6_6";
    case RHI::ShaderStage::AS: return L"as_6_6";
    case RHI::ShaderStage::LIB: return L"lib_6_5";
    default:                   return L"ps_6_6";
    }
}

CompileResult Compile(const void* src, std::size_t srcSize, const CompileOptions& opts)
{
    CompileResult out;

    EnsureInit();
    if (!g_initOk || !g_compiler || !g_utils)
    {
        out.errorMsg = "DXC not initialised";
        return out;
    }
    if (!src || srcSize == 0)
    {
        out.errorMsg = "empty source";
        return out;
    }

    // Build the wstring storage first, THEN derive the pointer array. A
    // tempting first attempt is to push to argStorage and append to args in
    // the same lambda, but std::vector<std::wstring> can reallocate on grow
    // and that invalidates every previously stored c_str(). Two passes keeps
    // it correct without playing reserve() games.
    std::vector<std::wstring> argStorage;
    argStorage.reserve(32);

    // Source name (must come first to be the "main file path")
    if (!opts.sourceName.empty())
        argStorage.emplace_back(Widen(opts.sourceName));

    // Entry + profile
    argStorage.emplace_back(L"-E");
    argStorage.emplace_back(Widen(opts.entry));
    argStorage.emplace_back(L"-T");
    argStorage.emplace_back(ProfileForStageW(opts.stage));

    // HLSL 2018 keeps the language behaviour close to what the existing SM 5.1
    // shaders were compiled against. Switch to 2021 only when we explicitly
    // want lvalue ternaries / strict overload resolution / etc.
    argStorage.emplace_back(L"-HV");
    argStorage.emplace_back(L"2018");

    // Auto-add the source file's parent directory as an -I path — covers the
    // common case where shaders/foo.ps.hlsl includes shaders/bar.hlsli.
    if (!opts.sourceName.empty())
    {
        std::filesystem::path p(opts.sourceName);
        std::filesystem::path parent = p.parent_path();
        if (!parent.empty())
        {
            argStorage.emplace_back(L"-I");
            argStorage.emplace_back(parent.wstring());
        }
    }

    if (!opts.includeDir.empty())
    {
        argStorage.emplace_back(L"-I");
        argStorage.emplace_back(Widen(opts.includeDir));
    }

    // Defines — DXC accepts -D NAME=VALUE (single arg, '=' inline).
    for (const Define& d : opts.defines)
    {
        std::string s = d.name;
        if (!d.value.empty())
        {
            s.push_back('=');
            s.append(d.value);
        }
        argStorage.emplace_back(L"-D");
        argStorage.emplace_back(Widen(s));
    }

    if (opts.debug)
    {
        argStorage.emplace_back(L"-Zi");
        argStorage.emplace_back(L"-Od");
    }
    else
    {
        argStorage.emplace_back(L"-O3");
    }

    std::vector<const wchar_t*> args;
    args.reserve(argStorage.size());
    for (const std::wstring& s : argStorage)
        args.push_back(s.c_str());

    // Wrap source bytes
    DxcBuffer srcBuf{};
    srcBuf.Ptr      = src;
    srcBuf.Size     = srcSize;
    srcBuf.Encoding = DXC_CP_UTF8;

    // AssetFS-backed include handler: resolves #include "X.hlsli" through
    // game.ipak (pak-first, loose-disk fallback) so runtime DXC compiles work in
    // PACKED builds. The stock CreateDefaultIncludeHandler reads the real
    // filesystem only, which made every live DDGI shader compile fail in a packed
    // build (the shared .hlsli headers live inside game.ipak).
    AssetFSIncludeHandler includeHandler;
    if (!opts.sourceName.empty())
        includeHandler.dirA = NormalizeKey(
            std::filesystem::path(opts.sourceName).parent_path().string());
    if (!opts.includeDir.empty())
        includeHandler.dirB = NormalizeKey(opts.includeDir);

    ComPtr<IDxcResult> result;
    HRESULT hr = g_compiler->Compile(&srcBuf,
                             args.data(), static_cast<UINT32>(args.size()),
                             &includeHandler,
                             IID_PPV_ARGS(&result));

    // Error/warning text — present even on success when there are warnings.
    if (result)
    {
        ComPtr<IDxcBlobUtf8> errBlob;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errBlob), nullptr))
            && errBlob && errBlob->GetStringLength() > 0)
        {
            out.errorMsg.assign(errBlob->GetStringPointer(),
                                errBlob->GetStringLength());
        }
    }

    if (FAILED(hr) || !result)
    {
        if (out.errorMsg.empty())
            out.errorMsg = "IDxcCompiler3::Compile call failed";
        return out;
    }

    HRESULT compileHr = S_OK;
    result->GetStatus(&compileHr);
    if (FAILED(compileHr))
    {
        // out.errorMsg already populated above.
        return out;
    }

    ComPtr<IDxcBlob> objBlob;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objBlob), nullptr))
        || !objBlob)
    {
        if (out.errorMsg.empty())
            out.errorMsg = "DXC compile produced no DXC_OUT_OBJECT";
        return out;
    }

    const uint8_t* p = static_cast<const uint8_t*>(objBlob->GetBufferPointer());
    out.dxil.assign(p, p + objBlob->GetBufferSize());
    out.ok = true;
    return out;
}

ID3D12ShaderReflection* CreateReflection(const void* container, std::size_t containerSize)
{
    EnsureInit();
    if (!g_initOk || !g_utils) return nullptr;
    if (!container || containerSize == 0) return nullptr;

    DxcBuffer buf{};
    buf.Ptr      = container;
    buf.Size     = containerSize;
    buf.Encoding = 0;   // binary container, no encoding hint

    ID3D12ShaderReflection* refl = nullptr;
    HRESULT hr = g_utils->CreateReflection(&buf, IID_PPV_ARGS(&refl));
    if (FAILED(hr))
    {
        LOG_ERROR("DxcCompiler::CreateReflection failed (hr=0x%08X)", hr);
        return nullptr;
    }
    return refl;
}

} // namespace DxcCompiler
