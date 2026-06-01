-- Afterimage.lua — press L to emit a dodge-ghost trail behind this entity.
--
-- Attach as a Logic ScriptComponent to a SKINNED character entity. The
-- ghosts are post-skinning vertex snapshots routed through the renderer's
-- AfterimageSystem (additive transparent + Fresnel rim PS).
--
-- Edge-triggered: pressing L fires ONE burst of 5 staggered snapshots over
-- ~240 ms. Re-pressing while a burst is still in flight is ignored until
-- the previous burst finishes (`self.busy` gate).
--
-- Tweakables:
--   GHOST_COUNT     — number of snapshots per burst
--   GHOST_INTERVAL  — seconds between consecutive snapshots
--   GHOST_LIFETIME  — seconds each ghost stays alive
--   GHOST_COLOR     — HDR rgb tint (>1.0 OK for emissive look)

local Afterimage = {}

local KEY_L = 0x4C  -- VK_L (Win32 virtual key code)

-- Each Spawn fans out to ALL sub-mesh entities of the character (body, hair,
-- clothes...). For a character with ~25 sub-meshes, GHOST_COUNT=3 uses ~75
-- pool slots — well within the 128-slot pool. Raise GHOST_COUNT only if you
-- have a single-mesh character; for many-submesh PMX/MMD models keep it low.
local GHOST_COUNT    = 5
local GHOST_INTERVAL = 0.08   -- 80 ms between snapshots
local GHOST_LIFETIME = 0.40   -- each ghost lives 400 ms
local GHOST_COLOR    = { r = 0.35, g = 0.85, b = 2.80 } -- cyan HDR

function Afterimage:OnSpawn(entity)
    self.entity   = entity
    self.lKeyDown = false   -- previous-frame edge state
    self.busy     = false   -- one burst at a time
    Log.Info("Afterimage ready on entity " .. tostring(entity))
end

function Afterimage:OnUpdate(dt)
    local nowDown = Input.IsKeyDown(KEY_L)

    -- Diagnostic: log every state transition. Remove once verified working.
    if nowDown ~= self.lKeyDown then
        Log.Info(string.format("Afterimage: L=%s busy=%s entity=%d",
            tostring(nowDown), tostring(self.busy), self.entity))
    end

    -- Edge: was up, now down → fire one burst.
    if nowDown and not self.lKeyDown and not self.busy then
        self:FireBurst()
    end
    self.lKeyDown = nowDown
end

function Afterimage:FireBurst()
    self.busy = true
    Log.Info("Afterimage: FireBurst start, entity=" .. self.entity)

    local e = self.entity
    -- First snapshot fires immediately so the player sees instant feedback.
    VFX.SpawnAfterimage(e, GHOST_LIFETIME, GHOST_COLOR.r, GHOST_COLOR.g, GHOST_COLOR.b)

    -- Remaining snapshots stagger via Engine.AfterDelay so each captures a
    -- different pose (the character continues animating between calls).
    for i = 1, GHOST_COUNT - 1 do
        Engine.AfterDelay(GHOST_INTERVAL * i, function()
            VFX.SpawnAfterimage(e, GHOST_LIFETIME,
                GHOST_COLOR.r, GHOST_COLOR.g, GHOST_COLOR.b)
        end)
    end

    -- Release the gate after the burst's total duration.
    Engine.AfterDelay(GHOST_INTERVAL * GHOST_COUNT, function()
        self.busy = false
    end)
end

function Afterimage:OnDestroy()
    self.busy = false
end

return Afterimage
