#include "PostProcess/PostProcessStack.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include <cassert>

namespace PostProcess
{

void Stack::RegisterEffect(std::unique_ptr<IEffect> effect)
{
    assert(effect && "RegisterEffect: null effect");
    const size_t slot = static_cast<size_t>(effect->GetStage());
    assert(slot < static_cast<size_t>(Stage::Count));
    m_effects[slot] = std::move(effect);
}

void Stack::Execute(Context& ctx)
{
    // Fall back to engine defaults if the caller didn't resolve this frame, so
    // adapters always see a valid contract.
    static const ResolvedPostProcessSettings kDefault{};
    if (!ctx.resolved) ctx.resolved = &kDefault;

    for (auto& effect : m_effects)
    {
        if (!effect) continue;
        if (!effect->IsEnabled(ctx)) continue;
        effect->Execute(ctx);
    }
}

} // namespace PostProcess
