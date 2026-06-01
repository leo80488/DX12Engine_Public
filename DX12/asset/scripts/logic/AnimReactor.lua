-- AnimReactor.lua — Logic script that handles animation events (doc §3.x).
--
-- Defines OnAnimEvent — the engine routes Engine.PublishAnimEvent(thisEntity,
-- name, payload) (and any future C++-side ScriptSystem::DispatchAnimEvent)
-- straight to this method.
--
-- Today the AnimationSystem doesn't auto-fire any events; gameplay code or a
-- future state-machine layer is expected to call PublishAnimEvent at the
-- right keyframes. This script is the receiving side.

local AnimReactor = {}

function AnimReactor:OnSpawn(entity)
    self.entity         = entity
    self.footstepCount  = 0
end

function AnimReactor:OnAnimEvent(name, payload)
    if name == "footstep" then
        self.footstepCount = self.footstepCount + 1
        -- payload may carry { foot = "left"|"right", surface = "stone"|... }
        local foot = (payload and payload.foot) or "?"
        Log.Info(string.format("[%s] footstep %d (%s)",
                               Engine.GetName(self.entity),
                               self.footstepCount, foot))

    elseif name == "attack_hit" then
        local damage = (payload and payload.damage) or 10
        Log.Info("Attack hit! damage=" .. damage)
        Engine.Publish("AttackLanded",
                       { source = self.entity, damage = damage })

    elseif name == "clip_end" then
        Log.Info("Clip finished on entity " .. self.entity)
    end
end

return AnimReactor
