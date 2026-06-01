-- ExposedDemo.lua — Logic script showcasing EDITOR-EXPOSED VARIABLES.
--
-- Declare a static `exposed` table on the returned template. The editor renders
-- one widget per entry in the Inspector (under the Script component), and each
-- entity stores its OWN edited values (serialized with the scene). Read them at
-- runtime straight off `self.<name>` — the engine injects the per-entity value
-- (or the schema default) onto the instance BEFORE OnSpawn fires.
--
-- Supported types: float, int, bool, vec3, color (hdr optional), string,
-- asset (ext-filtered drag-drop), entity. A BARE LITERAL infers its type:
--     speed = 1.0          -> float
--     loop  = true         -> bool
--     tag   = 'hello'      -> string
--     tint  = {1, 0, 0}    -> vec3
-- Use the explicit `{ type=..., default=..., min=..., max=..., tooltip=... }`
-- form when you want a slider range, a tooltip, an HDR color, or an asset ext.

local ExposedDemo = {}

ExposedDemo.exposed = {
    amplitude = { type = 'float',  default = 0.5, min = 0.0, max = 5.0,  tooltip = 'Bob height (world units)' },
    frequency = { type = 'float',  default = 2.0, min = 0.1, max = 10.0, tooltip = 'Oscillations per second' },
    animate   = { type = 'bool',   default = true,  tooltip = 'Toggle the up/down bob' },
    tint      = { type = 'color',  default = { 0.35, 0.85, 2.80 }, hdr = true, tooltip = 'Logged once on spawn' },
    greeting  = { type = 'string', default = 'Hello from ExposedDemo' },
}

function ExposedDemo:OnSpawn(entity)
    self.entity = entity
    self.base_y = Engine.GetBasePosition(entity).y
    -- self.amplitude / self.frequency / self.tint / ... are already populated
    -- by the engine from this entity's exposed-var values.
    Log.Info(string.format("%s  (amp=%.2f freq=%.2f tint=%.2f,%.2f,%.2f)",
        self.greeting, self.amplitude, self.frequency,
        self.tint.x, self.tint.y, self.tint.z))
end

function ExposedDemo:OnUpdate(dt)
    local t = Engine.GetLocalTransform(self.entity)
    if not t then return end

    -- Drift-free absolute write around the captured rest pose (see rotate.lua).
    local amp = self.animate and self.amplitude or 0.0
    local pos = t.translation
    pos.y = self.base_y + math.sin(Time.elapsed * self.frequency) * amp
    t.translation = pos
end

return ExposedDemo
