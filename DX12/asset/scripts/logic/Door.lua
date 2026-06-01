-- Door.lua — Logic script behaving as a Trigger (doc §3.x / §5.3).
--
-- Defines OnEnter, so the engine's ContactBegan bridge automatically calls
-- it whenever this entity collides with another body. No separate
-- TriggerComponent / TriggerVolume needed — any Logic script with OnEnter is
-- a trigger.
--
-- Attach this to an entity that has a physics body. When something touches
-- it, the door "opens" (one-shot, idempotent).

local Door = {}

function Door:OnSpawn(entity)
    self.entity = entity
    self.isOpen = false
    Log.Info("Door ready: " .. Engine.GetName(entity))
end

-- point / normal are tables {x,y,z} from the physics contact (nil if fired
-- via Engine.PublishTrigger from non-physics code).
function Door:OnEnter(other, point, normal)
    if self.isOpen then return end
    self.isOpen = true

    Log.Info("Door opened by entity " .. other)
    Engine.Publish("DoorOpened", { door = self.entity, opener = other })

    -- Re-arm after 3 seconds so subsequent crossings re-fire.
    Engine.AfterDelay(3.0, function()
        self.isOpen = false
    end)
end

return Door
