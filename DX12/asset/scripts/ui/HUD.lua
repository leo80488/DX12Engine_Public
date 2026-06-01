-- HUD.lua — UI script (doc §3.x / §6.2).
--
-- Returns a table with :Open(args) / :Close() conventions. Caller drives the
-- lifecycle:
--
--     local hud = Engine.GetUIScript("HUD")
--     hud:Open()              -- builds the widget tree, mounts as UI root
--     hud:UpdateFPS(60.0)     -- script-specific public API
--     hud:Close()             -- destroys the mounted UI root entity
--
-- Engine doesn't auto-mount UI scripts at boot — they're loaded into the
-- registry but only show when something calls :Open(). This matches the
-- "event-driven,跟 widget" lifecycle in doc §2.

local HUD = {}

function HUD:Open(args)
    if self.rootEntity then
        Log.Warning("HUD already open")
        return self.rootEntity
    end

    local size = (args and args.size) or {1920, 1080}

    local hud = ui.Canvas{ size = size }

    self.fpsText = ui.Text{
        parent = hud, anchor = "top-right", offset = {-20, 20},
        text = "FPS: --", color = 0xFFFFFFFF,
    }

    ui.Button{
        parent = hud, anchor = "bottom", offset = {0, -40},
        size = {200, 60}, text = "Pause",
        onClick = function()
            local s = Engine.GetTimeScale()
            Engine.SetTimeScale(s > 0.01 and 0.0 or 1.0)
        end,
    }

    self.rootEntity = ui.MountRoot("HUD", hud, 0)
    Log.Info("HUD opened (entity " .. self.rootEntity .. ")")
    return self.rootEntity
end

function HUD:Close()
    if not self.rootEntity then return end
    Engine.DestroyEntity(self.rootEntity)
    self.rootEntity = nil
    self.fpsText    = nil
    Log.Info("HUD closed")
end

-- Public API: call from a System / Logic each frame to refresh the FPS text.
function HUD:UpdateFPS(fps)
    if self.fpsText then
        self.fpsText:SetText(string.format("FPS: %.1f", fps))
    end
end

return HUD
