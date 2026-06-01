-- EnemyStates.lua — Logic ScriptComponent for an enemy with a 10-state FSM:
-- ATTACK_1/2/3, IDLE_1/2/3, WALK_1/2, RUN, TAKE_DAMAGE, DEATH.
--
-- Pairs with: asset/ai/enemy.bt.lua (transitions) and
--             asset/ai/enemy_actions.lua (BT actions that call SetState
--             AND publish Intent.MoveTo / Intent.LookAt / Intent.Stop).
--
-- This script ONLY registers animation states. The carrier entity also
-- needs a NavAgentComponent + CharacterControllerComponent (and an
-- AIIntentComponent if the BT publishes strategic goals via AI.SetGoal)
-- so the BT's movement intents actually drive the KCC kinematic body.
--
-- Edit STATE_CLIPS paths to match your project; clips are lazy-loaded the
-- first time the state becomes active.

local T = {}

local STATE_CLIPS = {
    IDLE_1      = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Idle1.ianim",   loop = true,  speed = 1.0 },
    IDLE_2      = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Idle2.ianim",   loop = true,  speed = 1.0 },
    IDLE_3      = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Idle3.ianim",   loop = true,  speed = 1.0 },

    WALK_1      = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Walk1.ianim",   loop = true,  speed = 1.0 },
    WALK_2      = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Walk2.ianim",   loop = true,  speed = 1.0 },

    -- Directional walks used by PrepareAttack when strafing around the
    -- player. Update the paths if your asset filenames differ.
    WALK_BACK   = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Walk_Back.ianim",  loop = true, speed = 1.0 },
    WALK_LEFT   = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Walk_Left.ianim",  loop = true, speed = 1.0 },
    WALK_RIGHT  = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Walk_Right.ianim", loop = true, speed = 1.0 },

    RUN         = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Run.ianim",       loop = true,  speed = 1.0 },

    ATTACK_1    = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Attack1.ianim", loop = false, speed = 1.0 },
    ATTACK_2    = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Attack2.ianim", loop = false, speed = 1.0 },
    ATTACK_3    = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Attack3.ianim", loop = false, speed = 1.0 },

    TAKE_DAMAGE = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Taking_Damage1.ianim", loop = false, speed = 1.0 },
    DEATH       = { path = "asset/SK_Whisper/Animations/Anim_Whisper_Death.ianim",     loop = false, speed = 1.0 },
}

-- The script attaches to the AI/physics root, but the AnimationComponent
-- lives on a skinned-mesh child. Resolve once and stash on `self` so the
-- whole FSM is wired against the child entity; fall back to root if the
-- character isn't nested (single-entity layout still works).
--
-- NOTE: Logic OnSpawn receives the entity id as a *number*, not a ctx
-- userdata (ScriptSystem::EnsureLogicInstance does `fn(inst, e)`).
local function ResolveAnimEntity(root)
    local found = Engine.FindAnimatedDescendant(root)
    if found and found ~= 0 then return found end
    return root
end

function T:OnSpawn(entity)
    self.animEntity = ResolveAnimEntity(entity)
    Log.Info(string.format(
        "EnemyStates: root=%d animEntity=%d", entity, self.animEntity))

    for name, cfg in pairs(STATE_CLIPS) do
        Character.AddState(self.animEntity, name, cfg.path,
                           { loop = cfg.loop, speed = cfg.speed })
    end
    -- Hard-cut into IDLE_1 so frame 0 doesn't fade from rest pose.
    Character.SetState(self.animEntity, "IDLE_1", 0)
end

return T
