#include "ECS/Scheduler.h"
#include "ECS/SystemRegistry.h"
#include "ECS/ISystem.h"
#include "ECS/FrameContext.h"
#include "ECS/CommandBuffer.h"
#include "ECS/ECS.h"
#include "System/TaskSystem.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{
    // Two systems conflict iff either is Opaque, OR they overlap on a
    // write (W-W), OR one writes what the other reads (R-W either
    // direction), OR they share an ExclusiveResource id.
    struct AccessSet
    {
        std::unordered_set<std::type_index> reads;
        std::unordered_set<std::type_index> writes;
        std::unordered_set<uint64_t>        exclusives;
        bool                                opaque = false;
    };

    bool Conflict(const AccessSet& a, const AccessSet& b)
    {
        if (a.opaque || b.opaque) return true;
        for (const auto& w : a.writes) if (b.writes.count(w)) return true;
        for (const auto& w : a.writes) if (b.reads.count(w))  return true;
        for (const auto& r : a.reads)  if (b.writes.count(r)) return true;
        for (const auto& e : a.exclusives) if (b.exclusives.count(e)) return true;
        return false;
    }
}

void Scheduler::RunPhase(TickPhase phase, World& world, FrameContext& ctx)
{
    if (!m_registry) return;

    const size_t  pIdx   = static_cast<size_t>(phase);
    const auto    systems = m_registry->GetSystems(phase);

    if (systems.empty())
    {
        m_phaseLastMs[pIdx] = 0.f;
        m_systemLastMs[pIdx].clear();
        return;
    }

    auto& sysMs = m_systemLastMs[pIdx];
    sysMs.assign(systems.size(), 0.f);

    using clock = std::chrono::steady_clock;
    using ms_f  = std::chrono::duration<float, std::milli>;

    const auto phaseStart = clock::now();

    const PhaseDescriptor& desc = kPhaseDescriptors[pIdx];
    const bool tryParallel = desc.allowsParallel
                          && !desc.requiresMainThread
                          && ctx.jobSystem
                          && systems.size() > 1;

    if (!tryParallel)
    {
        // Serial path — every system in registration order.
        for (size_t i = 0; i < systems.size(); ++i)
        {
            const auto sysStart = clock::now();
            systems[i]->Update(world, ctx);
            sysMs[i] = ms_f(clock::now() - sysStart).count();
        }
    }
    else
    {
        // Build (or reuse cached) per-phase batch plan.
        BatchPlan& plan = m_batchPlans[pIdx];
        const uint64_t curGen = m_registry->GetGeneration();
        if (plan.generation != curGen || plan.batches.empty())
        {
            plan.batches.clear();

            std::vector<AccessSet> access(systems.size());
            for (size_t i = 0; i < systems.size(); ++i)
            {
                SystemAccessBuilder b;
                systems[i]->DeclareAccess(b);
                access[i].reads      = b.Reads();
                access[i].writes     = b.Writes();
                access[i].exclusives = b.Exclusives();
                access[i].opaque     = b.IsOpaque();
            }

            // Linear-greedy: a system joins the current-last batch when
            // it doesn't conflict with anyone in it; otherwise starts a
            // new batch. Batch order = registration order — preserves
            // any "A produces what B reads" intent that wasn't captured
            // as a R-W declaration.
            for (size_t i = 0; i < systems.size(); ++i)
            {
                bool joined = false;
                if (!plan.batches.empty())
                {
                    bool conflictsWithBatch = false;
                    for (size_t inBatch : plan.batches.back())
                    {
                        if (Conflict(access[i], access[inBatch]))
                        {
                            conflictsWithBatch = true;
                            break;
                        }
                    }
                    if (!conflictsWithBatch)
                    {
                        plan.batches.back().push_back(i);
                        joined = true;
                    }
                }
                if (!joined)
                    plan.batches.push_back({ i });
            }
            plan.generation = curGen;
        }

        // Dispatch — one batch at a time, sync between.
        for (const auto& batch : plan.batches)
        {
            if (batch.size() == 1)
            {
                const size_t i = batch[0];
                const auto sysStart = clock::now();
                systems[i]->Update(world, ctx);
                sysMs[i] = ms_f(clock::now() - sysStart).count();
                continue;
            }

            // Parallel batch: dispatch each system as a High-priority task
            // and barrier on a local atomic + cv. We can't reuse
            // TaskSystem::WaitAll because that drains EVERY queued task,
            // not just our batch — would synchronise against unrelated
            // background work (resource loads, etc.).
            std::atomic<uint32_t>   remaining(static_cast<uint32_t>(batch.size()));
            std::mutex              doneMtx;
            std::condition_variable doneCv;

            for (size_t k = 0; k < batch.size(); ++k)
            {
                const size_t idx = batch[k];
                ctx.jobSystem->Push(
                    [&, idx]()
                    {
                        const auto sysStart = clock::now();
                        systems[idx]->Update(world, ctx);
                        sysMs[idx] = ms_f(clock::now() - sysStart).count();

                        std::lock_guard<std::mutex> lk(doneMtx);
                        remaining.fetch_sub(1, std::memory_order_relaxed);
                        doneCv.notify_one();
                    },
                    TaskSystem::TaskPriority::High);
            }

            std::unique_lock<std::mutex> lk(doneMtx);
            doneCv.wait(lk, [&] {
                return remaining.load(std::memory_order_acquire) == 0;
            });
        }
    }

    m_phaseLastMs[pIdx] = ms_f(clock::now() - phaseStart).count();

    // Flush deferred structural changes — phase boundaries are the only
    // place AddComponent/RemoveComponent/DestroyEntity are allowed to
    // mutate the live World. Doing it here means a system in phase[N+1]
    // sees the world that phase[N] decided to publish, never a partial
    // mid-iteration state.
    if (ctx.commandBuffer)
        ctx.commandBuffer->Flush(world);
}

std::vector<PhaseDebugInfo> Scheduler::GetDebugInfo() const
{
    std::vector<PhaseDebugInfo> out;
    out.reserve(static_cast<size_t>(TickPhase::COUNT));
    if (!m_registry) return out;

    for (size_t p = 0; p < static_cast<size_t>(TickPhase::COUNT); ++p)
    {
        const TickPhase phase  = static_cast<TickPhase>(p);
        const auto      systems = m_registry->GetSystems(phase);

        PhaseDebugInfo info;
        info.phase       = phase;
        info.name        = kPhaseDescriptors[p].name;
        info.lastFrameMs = m_phaseLastMs[p];
        info.systems.reserve(systems.size());

        const auto& sysMs = m_systemLastMs[p];
        for (size_t i = 0; i < systems.size(); ++i)
        {
            const float ms = (i < sysMs.size()) ? sysMs[i] : 0.f;
            info.systems.push_back({ systems[i]->GetName(), ms });
        }
        out.push_back(std::move(info));
    }
    return out;
}
