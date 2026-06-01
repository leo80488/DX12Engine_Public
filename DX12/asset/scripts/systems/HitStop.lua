-- HitStop.lua — System script (singleton).
--
-- Briefly freezes time on each physics contact for impact feel:
--   1. Subscribes to "ContactBegan" (auto-published by Physics on collision)
--   2. Sets Engine.SetTimeScale(near 0) to freeze game logic
--   3. Engine.AfterDelay(...) to restore — uses REAL time so it actually wakes
--
-- Other systems / logic scripts can drive an arbitrary freeze via:
--   Engine.GetSystem("HitStop"):Trigger(0.05, 0.08)

local HitStop = {}

-- Tweakables — change these and the file hot-reloads next frame:
local FREEZE_SCALE     = 0.05    -- 0 = full freeze; 0.05 leaves a tiny breath
local FREEZE_DURATION  = 0.08    -- 80ms — feels like a punch landing
local REQUIRE_TAGS     = false   -- true to only fire on tagged contacts
local TAG_ATTACKER     = "weapon"
local TAG_VICTIM       = "enemy"

local function shouldFreeze(ev)
    if not REQUIRE_TAGS then return true end
    return  Engine.HasTag(ev.bodyA, TAG_ATTACKER)
        and Engine.HasTag(ev.bodyB, TAG_VICTIM)
end

function HitStop:OnInit()
    -- Generation counter so simultaneous hits don't restore early. Without
    -- this: hit A starts 80ms timer; hit B (10ms later) starts another; A's
    -- timer fires at 80ms and restores scale, even though B wanted 90ms total.
    self.activeStops = 0

    self.subId = Engine.Subscribe("ContactBegan", function(ev)
        if not shouldFreeze(ev) then return end
        self:Trigger(FREEZE_SCALE, FREEZE_DURATION)
    end)
end

function HitStop:OnShutdown()
    if self.subId then Engine.Unsubscribe(self.subId) end
    Engine.SetTimeScale(1.0)
end

-- Public API: Engine.GetSystem("HitStop"):Trigger(0.05, 0.08)
function HitStop:Trigger(scale, duration)
    scale    = scale    or FREEZE_SCALE
    duration = duration or FREEZE_DURATION

    Engine.SetTimeScale(scale)
    self.activeStops = self.activeStops + 1

    Engine.AfterDelay(duration, function()
        self.activeStops = self.activeStops - 1
        if self.activeStops <= 0 then
            self.activeStops = 0
            Engine.SetTimeScale(1.0)
        end
    end)
end

return HitStop
