-- rotate.lua — oscillate Y position as a sine wave around the entity's
-- starting position. Demonstrates the drift-free pattern:
--
--   * capture the baseline ONCE in Init via GetBasePosition()
--   * write ABSOLUTE positions every frame: pos.y = base.y + sin(t) * amp
--
-- NEVER integrate deltas like `pos.y = pos.y + sin(t) * amp * dt`. That form
-- accumulates floating-point error and, crucially, has no memory of where
-- "home" is — pausing and resuming the script leaves the entity wherever it
-- happened to be at pause-time, and the oscillation silently drifts off.

local amplitude = 0.5
local frequency = 2.0

local base_y = 0.0

function Init()
    Log.Info("rotate.lua Init() for entity: " .. GetName())
    base_y = GetBasePosition().y
end

function Update(dt)
    local t = GetLocalTransform()
    if not t then return end

    local pos = t.translation
    pos.y = base_y + math.sin(Time.elapsed * frequency) * amplitude
    t.translation = pos
end
