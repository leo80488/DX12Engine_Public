#include "Graphics/DxcCompiler.h"

#include "System/Log.h"

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

    // Default include handler: resolves "X.hlsli" first against the source's
    // virtual path, then against -I directories. Each Compile() call gets a
    // fresh handler so DXC can keep its include cache scoped per compile.
    ComPtr<IDxcIncludeHandler> includeHandler;
    HRESULT hr = g_utils->CreateDefaultIncludeHandler(&includeHandler);
    if (FAILED(hr))
    {
        out.errorMsg = "CreateDefaultIncludeHandler failed";
        return out;
    }

    ComPtr<IDxcResult> result;
    hr = g_compiler->Compile(&srcBuf,
                             args.data(), static_cast<UINT32>(args.size()),
                             includeHandler.Get(),
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
