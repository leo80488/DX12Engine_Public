-- CharacterStates.lua — registers a four-state animation FSM
-- (IDLE / WALK / RUN / ATTACK) and enters IDLE on spawn. Attach as a
-- Logic ScriptComponent to a skinned character. A BT or another Lua
-- system drives transitions via Actions.SetState / Character.SetState.
--
-- Author the asset paths to match your project. The clip files don't
-- need to exist yet — Character.AddState records the path; the
-- CharacterStateSystem lazy-acquires + binds when each state first
-- becomes active.
--
-- Tweak `blend` on SetState calls (here or in the BT) to control how
-- snappy the transition feels. 0.0 = hard cut, 0.2 = typical, 0.5 = soft.

local T = {}

-- Edit these paths to match your project layout.
local STATE_CLIPS = {
    IDLE   = { path = "asset/anims/idle.ianim",   loop = true,  speed = 1.0 },
    WALK   = { path = "asset/anims/walk.ianim",   loop = true,  speed = 1.0 },
    RUN    = { path = "asset/anims/run.ianim",    loop = true,  speed = 1.2 },
    ATTACK = { path = "asset/anims/attack.ianim", loop = false, speed = 1.0 },
}

function T:OnSpawn(ctx)
    local e = ctx:Entity()
    for name, cfg in pairs(STATE_CLIPS) do
        Character.AddState(e, name, cfg.path, { loop = cfg.loop, speed = cfg.speed })
    end
    -- Enter IDLE with no blend so the first frame doesn't fade from
    -- nothing → IDLE (which would briefly show the rest pose).
    Character.SetState(e, "IDLE", 0)
end

-- Optional: expose `self.NextState(name, blend)` for other scripts to call
-- via the entity's per-entity Lua environment.
function T:NextState(ctx, name, blend)
    return Character.SetState(ctx:Entity(), name, blend or 0.2)
end

return T
