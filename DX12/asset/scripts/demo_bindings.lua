-- demo_bindings.lua — Logic script that exercises various Engine.* bindings.
-- Attach to any entity to verify each API works at runtime.

local DemoBindings = {}

function DemoBindings:OnSpawn(entity)
    self.entity = entity
    self.myCamera = 0  -- fill with a camera entity ID at runtime if you want
    Log.Info("demo_bindings: OnSpawn on entity " .. entity)
end

function DemoBindings:OnUpdate(dt)
    local id = self.entity

    -- ---- Keyboard + Mouse --------------------------------------------------
    if Input.IsMouseDown(Mouse.Left) then
        Log.Info("Left mouse held")
    end
    -- local mx, my = Input.GetMousePos()  -- silent unless you want spam

    -- ---- Pulsing light on this entity (if it has one) ---------------------
    if Engine.HasComponent(id, "Light") then
        local light = Engine.GetLight(id)
        if light then
            light.intensity = 1.0 + math.sin(Time.elapsed * 3.0) * 0.5
        end
    end

    -- ---- Toggle-animation demo --------------------------------------------
    local anim = Engine.GetAnimation(id)
    if anim and Input.IsKeyDown(Keys.Space) then
        anim.paused = not anim.paused
    end

    -- ---- Zoom camera by ID (set self.myCamera above to a Main Camera id) --
    -- if self.myCamera ~= 0 then
    --     local cam = Engine.GetCamera(self.myCamera)
    --     if cam then
    --         cam.fov = 1.047 + math.sin(Time.elapsed) * 0.3
    --     end
    -- end
end

return DemoBindings
