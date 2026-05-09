#pragma once

// PhysicsSystem — ECS bridge for Jolt Physics.
//
// Runs a fixed 60Hz simulation via an accumulator inside Update(). Creates
// Jolt bodies lazily for entities that have both RigidBodyComponent and
// ColliderComponent. After each simulation step, writes dynamic/kinematic
// body transforms back into LocalTransform so TransformSystem::Propagate
// sees fresh data.
//
// Ticking order inside Scene::Update() must be:
//   1) scripts / game logic
//   2) PhysicsSystem::Update(world, dt)
//   3) TransformSystem::Propagate(world)
//
// Jolt types are fully hidden via pImpl so this header stays cheap to include.

#include <memory>

class World;

namespace DX12Physics
{
    class PhysicsSystem
    {
    public:
        PhysicsSystem();
        ~PhysicsSystem();

        PhysicsSystem(const PhysicsSystem&)            = delete;
        PhysicsSystem& operator=(const PhysicsSystem&) = delete;

        // Init: registers Jolt factory/types, spins up job pool, creates the
        // JPH::PhysicsSystem. Safe to call once per process (scenes share the
        // Jolt factory state).
        void Init();
        void Shutdown();

        // Variable dt comes in; internally drives the Jolt system at fixed 60Hz.
        void Update(World& world, float dt);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
