-- QuestSystem.lua — System script (singleton, doc §3.3).
--
-- Owns the global active-quest list. Subscribes to gameplay events and
-- updates quest progress; exposes AddQuest / CompleteQuest as a public API
-- accessible from other scripts via Engine.GetSystem("QuestSystem").

local QuestSystem = {}

function QuestSystem:OnInit()
    self.activeQuests = {}        -- id -> { name, progress, target }
    self.nextId       = 1

    -- Subscribe to game events. Store the sub IDs so OnShutdown can detach.
    self.subs = {}

    -- Example: bump progress on every contact for "punching bag" quests.
    self.subs.contact = Engine.Subscribe("ContactBegan", function(ev)
        for id, q in pairs(self.activeQuests) do
            if q.kind == "hit" then
                q.progress = q.progress + 1
                if q.progress >= q.target then
                    self:CompleteQuest(id)
                end
            end
        end
    end)

    Log.Info("QuestSystem online")
end

function QuestSystem:OnUpdate(dt)
    -- Most quest logic is event-driven; OnUpdate is here in case you want
    -- timed quests, hint-display countdowns, etc.
end

function QuestSystem:OnShutdown()
    for _, subId in pairs(self.subs) do Engine.Unsubscribe(subId) end
    Log.Info("QuestSystem shutdown")
end

-- ---- Public API ---------------------------------------------------------
-- Engine.GetSystem("QuestSystem"):AddQuest({ name = "Punch 5 things", kind = "hit", target = 5 })
function QuestSystem:AddQuest(def)
    local id = self.nextId
    self.nextId = id + 1
    self.activeQuests[id] = {
        name     = def.name or ("Quest " .. id),
        kind     = def.kind or "generic",
        target   = def.target or 1,
        progress = 0,
    }
    Log.Info("Quest started: " .. self.activeQuests[id].name)
    return id
end

function QuestSystem:CompleteQuest(id)
    local q = self.activeQuests[id]
    if not q then return end
    Log.Info("Quest complete: " .. q.name)
    self.activeQuests[id] = nil
    Engine.Publish("QuestCompleted", { id = id, name = q.name })
end

return QuestSystem
