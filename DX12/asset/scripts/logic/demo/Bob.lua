-- Bob.lua — Logic script (per-entity instance).
--
-- Half of the MULTI-SCRIPT demo: oscillates the entity's Y position as a sine
-- wave around its rest pose. Designed to share an entity with Spin.lua — Bob
-- only ever writes translation.y, Spin only writes rotation, so the two run
-- independently on the same cube (it bobs AND spins at once).
--
-- Drift-free pattern (see rotate.lua): capture the baseline ONCE and write
-- ABSOLUTE positions every frame, never integrate deltas.

local Bob = {}

-- Editor-exposed knobs — tweak per-entity in the Inspector (Script component).
Bob.exposed = {
    amplitude = { type = 'float', default = 0.6, min = 0.0, max = 5.0,  tooltip = 'Bob height (world units)' },
    frequency = { type = 'float', default = 2.0, min = 0.1, max = 10.0, tooltip = 'Angular speed (radians/sec)' },
}

function Bob:OnSpawn(entity)
    self.entity = entity
    self.base_y = Engine.GetBasePosition(entity).y
    Log.Info(string.format("Bob.lua spawned on slot %s of '%s'",
        tostring(self.slot), Engine.GetName(entity)))
end

function Bob:OnUpdate(dt)
    local t = Engine.GetLocalTransform(self.entity)
    if not t then return end

    local pos = t.translation
    pos.y = self.base_y + math.sin(Time.elapsed * self.frequency) * self.amplitude
    t.translation = pos
end

return Bob
