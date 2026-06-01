#pragma once

// Scheduler — runs one TickPhase's worth of systems against the World.
//
// EngineLoop calls RunPhase() per phase in execution order; the Scheduler
// dispatches the systems registered in that phase (serially in S0; with
// per-phase parallel batches via DeclareAccess and TaskSystem in S3+),
// then flushes the FrameContext's CommandBuffer so deferred structural
// changes land before the next phase starts.
//
// Per-system / per-phase timing is captured every frame so the Editor can
// render a Phase Debug panel (design §8.1) without poking the registry
// directly.

#include "TickPhase.h"

#include <array>
#include <vector>

class World;
struct FrameContext;
class SystemRegistry;

struct SystemDebugInfo
{
    const char* name        = "";
    float       lastFrameMs = 0.f;
};

struct PhaseDebugInfo
{
    TickPhase   phase       = TickPhase::Input;
    const char* name        = "";
    float       lastFrameMs = 0.f;
    std::vector<SystemDebugInfo> systems;
};

class Scheduler
{
public:
    void SetRegistry(SystemRegistry* reg) { m_registry = reg; }

    // Runs every system in `phase` once. No-op if the phase has no
    // systems registered, or if no registry is bound. Records last-frame
    // ms per system + per phase for GetDebugInfo().
    void RunPhase(TickPhase phase, World& world, FrameContext& ctx);

    // Snapshot of every phase's last-frame timing. Phases with no
    // registered systems still appear with lastFrameMs = 0 + empty
    // systems vector — caller filters on .systems.empty() to hide them.
    std::vector<PhaseDebugInfo> GetDebugInfo() const;

private:
    SystemRegistry* m_registry = nullptr;

    // Last-frame totals, indexed by static_cast<size_t>(TickPhase).
    std::array<float, static_cast<size_t>(TickPhase::COUNT)> m_phaseLastMs{};
    // Last-frame per-system ms, parallel to the registry's system span
    // for each phase. Sized lazily by RunPhase.
    std::array<std::vector<float>, static_cast<size_t>(TickPhase::COUNT)> m_systemLastMs;

    // Per-phase parallel batch plan (S3). batches[k] = system indices that
    // can run concurrently. Linear-greedy build: a system joins the
    // CURRENT-LAST batch when it doesn't conflict with anyone in it,
    // otherwise starts a new batch — guarantees batch order matches
    // registration order. Cached by SystemRegistry::GetGeneration().
    struct BatchPlan
    {
        uint64_t                          generation = 0;
        std::vector<std::vector<size_t>>  batches;
    };
    mutable std::array<BatchPlan, static_cast<size_t>(TickPhase::COUNT)> m_batchPlans;
};
