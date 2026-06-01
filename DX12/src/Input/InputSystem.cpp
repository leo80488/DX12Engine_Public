#include "Input/InputSystem.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

Input& Input::Get() noexcept
{
    static Input instance;
    return instance;
}

void Input::Update()
{
    std::memcpy(m_prev, m_curr, sizeof(m_curr));
    // GetAsyncKeyState returns a SHORT. Bit 0x8000 = currently down; bit 0x1
    // = transition flag (set if pressed since last call) — we don't use the
    // transition flag because our own prev/curr table provides edge detection
    // that stays consistent across systems reading input within one frame.
    for (int vk = 0; vk < kNumKeys; ++vk)
        m_curr[vk] = (GetAsyncKeyState(vk) & 0x8000) ? std::uint8_t(1) : std::uint8_t(0);
}
