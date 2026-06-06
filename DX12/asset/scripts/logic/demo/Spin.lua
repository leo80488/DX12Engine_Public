-- Spin.lua — Logic script (per-entity instance).
--
-- Other half of the MULTI-SCRIPT demo: continuously rotates the entity about an
-- axis, composed onto its rest pose. Shares an entity with Bob.lua — Spin only
-- writes rotation, Bob only writes translation.y, so both scripts drive the
-- same cube at the same time without stepping on each other.
--
-- Drift-free pattern: the angle is derived from absolute elapsed time (not
-- accumulated per frame), so pausing/resuming never drifts.

local Spin = {}

Spin.exposed = {
    speed = { type = 'float', default = 1.5, min = -8.0, max = 8.0, tooltip = 'Spin speed (radians/sec)' },
    axis  = { type = 'vec3',  default = { 0.0, 1.0, 0.0 },          tooltip = 'Rotation axis (auto-normalized)' },
}

-- Hamilton product a (x) b → x,y,z,w. Lua's Quat usertype exposes no operators,
-- so we compose quaternions by hand to layer the spin onto the rest rotation.
local function quatMul(ax, ay, az, aw, bx, by, bz, bw)
    return aw*bx + ax*bw + ay*bz - az*by,
           aw*by - ax*bz + ay*bw + az*bx,
           aw*bz + ax*by - ay*bx + az*bw,
           aw*bw - ax*bx - ay*by - az*bz
end

function Spin:OnSpawn(entity)
    self.entity = entity

    -- Rest rotation to spin around (captured once, shared with any other script
    -- on this entity).
    local r = Engine.GetBaseRotation(entity)
    self.bx, self.by, self.bz, self.bw = r.x, r.y, r.z, r.w

    -- Normalize the exposed axis once (self.axis is injected as a Vec3).
    local ax, ay, az = self.axis.x, self.axis.y, self.axis.z
    local len = math.sqrt(ax*ax + ay*ay + az*az)
    if len < 1e-6 then ax, ay, az, len = 0.0, 1.0, 0.0, 1.0 end
    self.ax, self.ay, self.az = ax/len, ay/len, az/len

    Log.Info(string.format("Spin.lua spawned on slot %s of '%s'",
        tostring(self.slot), Engine.GetName(entity)))
end

function Spin:OnUpdate(dt)
    local t = Engine.GetLocalTransform(self.entity)
    if not t then return end

    -- Absolute angle from elapsed time → drift-free.
    local half = Time.elapsed * self.speed * 0.5
    local s = math.sin(half)
    local dx, dy, dz, dw = self.ax * s, self.ay * s, self.az * s, math.cos(half)

    -- restPose (x) spinDelta. Mutate the returned Quat and write it back —
    -- the engine's Quat usertype isn't callable as a constructor, so we use the
    -- same read-mutate-write pattern as translation (see Bob.lua).
    local q = t.rotation
    q.x, q.y, q.z, q.w = quatMul(self.bx, self.by, self.bz, self.bw, dx, dy, dz, dw)
    t.rotation = q
end

return Spin
