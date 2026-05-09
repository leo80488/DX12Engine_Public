-- hit_stop.lua — global script: pause time briefly on each hit for impact feel.
--
-- Wires:
--   1. Subscribes to "ContactBegan" (auto-published by Physics on collision)
--   2. Calls Engine.SetTimeScale(near 0) to freeze game logic
--   3. Engine.AfterDelay(...) to restore — uses REAL time so it actually wakes
--
-- Tweakables — change these and the script hot-reloads next frame:
local FREEZE_SCALE     = 0.05    -- 0 = full freeze; 0.05 leaves a tiny breath
local FREEZE_DURATION  = 0.08    -- 80ms — feels like a punch landing
local REQUIRE_TAGS     = false   -- set true to only fire on tagged contacts
local TAG_ATTACKER     = "weapon"
local TAG_VICTIM       = "enemy"

-- Generation counter so simultaneous hits don't restore early.
-- Without this: hit A starts 80ms timer; hit B (10ms later) starts another;
-- A's timer fires at 80ms and restores scale, even though B wanted 90ms total.
local activeStops = 0

local function shouldFreeze(ev)
    if not REQUIRE_TAGS then return true end
    return  Engine.HasTag(ev.bodyA, TAG_ATTACKER)
        and Engine.HasTag(ev.bodyB, TAG_VICTIM)
end

Engine.Subscribe("ContactBegan", function(ev)
    if not shouldFreeze(ev) then return end

    Engine.SetTimeScale(FREEZE_SCALE)
    activeStops = activeStops + 1

    Engine.AfterDelay(FREEZE_DURATION, function()
        activeStops = activeStops - 1
        if activeStops <= 0 then
            activeStops = 0
            Engine.SetTimeScale(1.0)
        end
    end)
end)

-- Optional: manual trigger you can call from another script for non-physics hits.
function TriggerHitStop(scale, duration)
    scale    = scale    or FREEZE_SCALE
    duration = duration or FREEZE_DURATION
    Engine.SetTimeScale(scale)
    activeStops = activeStops + 1
    Engine.AfterDelay(duration, function()
        activeStops = activeStops - 1
        if activeStops <= 0 then
            activeStops = 0
            Engine.SetTimeScale(1.0)
        end
    end)
end
