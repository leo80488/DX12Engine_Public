#pragma once

// Phase 1 adapters — wrap existing compute passes (owned by Renderer) as
// PostProcess::IEffect so they can be driven through the Stack. Adapters
// are non-owning: the underlying passes stay lifetime-managed by Renderer.
//
// Each adapter does exactly what the corresponding block in
// Renderer::Render() Phase 4.7 used to do:
//   - reads relevant SRV handles from the Context
//   - configures the wrapped pass
//   - calls pass->Execute(ctx.cl)
//   - writes any produced SRV handles back into the Context

#include "PostProcess/IPostProcessEffect.h"

class CASPass;
class AutoExposurePass;
class BloomPass;
class LensFlarePass;
class ToneMapPass;
class UnderwaterPass;
class DepthOfFieldPass;
class StylizePass;

namespace PostProcess
{

class DepthOfFieldEffect final : public IEffect
{
public:
    explicit DepthOfFieldEffect(DepthOfFieldPass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::DepthOfField; }
    const char* GetName()  const override { return "DepthOfField"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    DepthOfFieldPass* m_pass = nullptr;
};

class CASEffect final : public IEffect
{
public:
    explicit CASEffect(CASPass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::CAS; }
    const char* GetName()  const override { return "CAS"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    CASPass* m_pass = nullptr;
};

class AutoExposureEffect final : public IEffect
{
public:
    explicit AutoExposureEffect(AutoExposurePass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::AutoExposure; }
    const char* GetName()  const override { return "AutoExposure"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    AutoExposurePass* m_pass = nullptr;
};

class BloomEffect final : public IEffect
{
public:
    explicit BloomEffect(BloomPass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::Bloom; }
    const char* GetName()  const override { return "Bloom"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    BloomPass* m_pass = nullptr;
};

class LensFlareEffect final : public IEffect
{
public:
    explicit LensFlareEffect(LensFlarePass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::LensFlare; }
    const char* GetName()  const override { return "LensFlare"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    LensFlarePass* m_pass = nullptr;
};

class UnderwaterEffect final : public IEffect
{
public:
    explicit UnderwaterEffect(UnderwaterPass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::Underwater; }
    const char* GetName()  const override { return "Underwater"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    UnderwaterPass* m_pass = nullptr;
};

class StylizeEffect final : public IEffect
{
public:
    explicit StylizeEffect(StylizePass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::Stylize; }
    const char* GetName()  const override { return "Stylize"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    StylizePass* m_pass = nullptr;
};

class ToneMapEffect final : public IEffect
{
public:
    explicit ToneMapEffect(ToneMapPass* pass) : m_pass(pass) {}
    Stage       GetStage() const override { return Stage::Tonemapping; }
    const char* GetName()  const override { return "Tonemapping"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    ToneMapPass* m_pass = nullptr;
};

} // namespace PostProcess
