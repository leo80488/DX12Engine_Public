#include "ECS/SystemRegistry.h"
#include "ECS/ECS.h"

// Tick-order registration for engine systems lives in App::Run (search
// for "// === System Registration ===") — adapters need refs to App's
// owned concrete singletons (ScriptSystem, PhysicsSystem, ...) which
// can't be plumbed through a static factory cleanly.

void SystemRegistry::Initialize(World& world)
{
    for (auto& phaseVec : m_phases)
        for (auto& sys : phaseVec)
            sys->OnRegister(world);
}

void SystemRegistry::Shutdown(World& world)
{
    // Reverse-iterate so OnUnregister mirrors OnRegister order. Systems
    // late in the list often subscribed to events fired by earlier ones;
    // tearing them down in reverse keeps that dependency direction.
    for (auto it = m_phases.rbegin(); it != m_phases.rend(); ++it)
        for (auto sit = it->rbegin(); sit != it->rend(); ++sit)
            (*sit)->OnUnregister(world);
}

std::span<const std::unique_ptr<ISystem>>
SystemRegistry::GetSystems(TickPhase phase) const
{
    return m_phases[static_cast<size_t>(phase)];
}
