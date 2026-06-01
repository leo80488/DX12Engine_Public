#pragma once

// InputSystem — centralised keyboard polling.
//
// Replaces scattered GetAsyncKeyState() calls across CameraSystem, ScriptSystem,
// Scene transitions, etc. Per-frame Update() snapshots the OS keyboard into a
// 256-byte table and rolls the previous snapshot so edge queries (WasPressed /
// WasReleased) are cheap and consistent across all systems within a frame.
//
// Threading: single-threaded. Update() is called once from the App tick BEFORE
// any system reads input. Queries are const and safe from any thread, but they
// see the snapshot taken at the most recent Update() call.
//
// Mouse: this header covers keyboard only. The existing System/Mouse.h handles
// mouse state (event-driven, plays well with ImGui). Cursor delta / viewport
// drag plumbing stays where it is in App.cpp / EditorLayer.

#include <cstdint>
#include <cstring>

class Input
{
public:
    static Input& Get() noexcept;

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    // Roll prev → curr and snapshot the OS keyboard into the curr buffer.
    // Call once per frame at the START of the App tick, before any system
    // reads input — otherwise WasKeyPressed/WasKeyReleased are unreliable.
    void Update();

    // Clear both prev and curr to zero. Use when the window loses focus, to
    // avoid "stuck" keys when the user releases a key while another window
    // owns the keyboard.
    void Reset() noexcept
    {
        std::memset(m_curr, 0, sizeof(m_curr));
        std::memset(m_prev, 0, sizeof(m_prev));
    }

    // Hold queries. @p vk is a Windows virtual-key code (VK_*), or an ASCII
    // upper-case letter / digit for keyboard keys (e.g. 'W', '0').
    bool IsKeyDown(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] != 0;
    }

    // Edge queries (false except on the single frame of the transition).
    // Reliable only when Update() ran exactly once per frame.
    bool WasKeyPressed(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] != 0 && m_prev[vk] == 0;
    }
    bool WasKeyReleased(int vk) const noexcept
    {
        return (vk >= 0 && vk < kNumKeys) && m_curr[vk] == 0 && m_prev[vk] != 0;
    }

    // 1D axis helper. Result is (posVk ? 1 : 0) - (negVk ? 1 : 0). Returns
    // -1, 0, or +1. Convenience for movement code: Axis('D','A') reads strafe.
    float Axis(int posVk, int negVk) const noexcept
    {
        return (IsKeyDown(posVk) ? 1.f : 0.f) - (IsKeyDown(negVk) ? 1.f : 0.f);
    }

private:
    Input() = default;

    static constexpr int kNumKeys = 256;
    std::uint8_t m_curr[kNumKeys] = {};
    std::uint8_t m_prev[kNumKeys] = {};
};
