-- rotate.lua — Logic script (per-entity instance).
--
-- Oscillates the entity's Y position as a sine wave around the starting
-- position. Demonstrates the drift-free pattern:
--
--   * capture the baseline ONCE in OnSpawn via Engine.GetBasePosition(self.entity)
--   * write ABSOLUTE positions every frame: pos.y = base.y + sin(t) * amp
--
-- NEVER integrate deltas like `pos.y = pos.y + sin(t) * amp * dt`. That form
-- accumulates floating-point error and, crucially, has no memory of where
-- "home" is — pausing and resuming the script leaves the entity wherever it
-- happened to be at pause-time, and the oscillation silently drifts off.

local Rotate = {}

local AMPLITUDE = 0.5
local FREQUENCY = 2.0

function Rotate:OnSpawn(entity)
    -- self.entity is auto-set by the engine; storing it here just for clarity.
    self.entity = entity
    self.base_y = Engine.GetBasePosition(entity).y
    Log.Info("rotate.lua OnSpawn for entity " .. Engine.GetName(entity))
end

function Rotate:OnUpdate(dt)
    local t = Engine.GetLocalTransform(self.entity)
    if not t then return end

    local pos = t.translation
    pos.y = self.base_y + math.sin(Time.elapsed * FREQUENCY) * AMPLITUDE
    t.translation = pos
end

return Rotate
