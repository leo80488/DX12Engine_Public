#pragma once

// PostProcess::Stack — fixed-slot container that executes IEffect instances in
// Stage order. It is a pure CONSUMER of the volume/profile system's output:
// each frame the Renderer points Context::resolved at the frame's
// ResolvedPostProcessSettings (produced by PostProcessResolveSystem) and the
// adapters read their fields from it. The Stack itself knows nothing about
// volumes, profiles, blending, or overrides — that all happens upstream in the
// ECS resolve, keeping the two layers fully decoupled (see
// DesignMd/PostProcessVolume_Architecture.md §1).

#include "PostProcess/IPostProcessEffect.h"

#include <array>
#include <memory>

namespace PostProcess
{

class Stack
{
public:
    Stack() = default;
    ~Stack() = default;

    Stack(const Stack&)            = delete;
    Stack& operator=(const Stack&) = delete;

    // Registers `effect` at its GetStage() slot. Overwrites any previously
    // registered effect for the same slot (last-writer-wins — fine for our
    // fixed 1:1 stage→adapter mapping).
    void RegisterEffect(std::unique_ptr<IEffect> effect);

    // Walks stages in enum order, calling Execute() on each enabled effect.
    // `ctx.cl` must be a valid compute command list recorded serially on a
    // single thread, and `ctx.resolved` must point at the frame's resolved
    // settings (the Renderer sets it before calling).
    void Execute(Context& ctx);

    // Direct slot query — useful for EditorLayer debug UI / introspection.
    IEffect* GetEffect(Stage s) const
    {
        return m_effects[static_cast<size_t>(s)].get();
    }

private:
    std::array<std::unique_ptr<IEffect>,
               static_cast<size_t>(Stage::Count)> m_effects{};
};

} // namespace PostProcess
