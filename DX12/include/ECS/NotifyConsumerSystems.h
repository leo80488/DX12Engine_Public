#pragma once

// NotifyConsumerSystems — six skeleton systems that drain the Pending*
// mailbox components written by TimelineSystem.
//
// Tier 1 (strongly-typed, engine subsystem):
//   HitboxSystem            — consumes PendingHitboxCommands
//   VFXSpawnSystem          — consumes PendingVFXSpawns
//   CameraEffectSystem      — consumes PendingCameraEffects
//   AudioPlaySystem         — consumes PendingAudioPlays
// Tier 2 (generic state):
//   StateToggleSystem       — consumes PendingStateToggles
// Tier 3 (designer/Lua bridge):
//   GenericNotifyDispatcher — consumes PendingGenericNotifies → EventBus
//
// All six live in TickPhase::BoneAttachment so they fire in the same
// render frame that TimelineSystem (TickPhase::Animation) wrote the
// mailbox. AudioPlaySystem republishes to the existing EventBus
// PlaySoundEvent so the present AudioSystem doesn't need to know about
// notifies at all.
//
// Today the Hitbox / VFX / Camera consumers log + clear; their downstream
// engine subsystems (collision sweep, VFX asset → ParticleSystem, camera
// shake driver) are wired in later stages.

#include "ECS/ISystem.h"

class HitboxSystem            final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "HitboxSystem"; }            void Update(World&, const FrameContext&) override; };

class VFXSpawnSystem          final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "VFXSpawnSystem"; }          void Update(World&, const FrameContext&) override; };

class CameraEffectSystem      final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "CameraEffectSystem"; }      void Update(World&, const FrameContext&) override; };

class AudioPlaySystem         final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "AudioPlaySystem"; }         void Update(World&, const FrameContext&) override; };

class StateToggleSystem       final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "StateToggleSystem"; }       void Update(World&, const FrameContext&) override; };

class GenericNotifyDispatcher final : public SystemInPhase<TickPhase::BoneAttachment>
{ public: const char* GetName() const override { return "GenericNotifyDispatcher"; } void Update(World&, const FrameContext&) override; };
