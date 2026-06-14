#pragma once

// Lua bindings for the post-process volume system. "Lua expresses intent, C++
// implements" (design §7): scripts spawn volumes, tweak profile properties, push
// transient gameplay overrides, and query resolved values — but all blending /
// spatial query / resolve stays in C++.

namespace sol { class state; }
class World;

namespace PostProcess
{
    // Registers the global `PostProcess` table. Captures @p world for volume
    // entity spawn/lookup; profiles + override stack are reached via singletons
    // (ProfileSystem / Runtime).
    void RegisterLuaPostProcessBindings(sol::state& lua, World& world);
}
