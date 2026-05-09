-- demo_bindings.lua — exercises the bindings added in the Lua pass 1 expansion.
-- Attach this script to any entity to verify each API works at runtime.

local myCamera = 0   -- fill with camera entity id at runtime if you want

function Init()
    Log.Info("demo_bindings: Init on entity " .. GetEntityID())
end

function Update(dt)
    -- ---- Keyboard (existing) + Mouse (new) --------------------------------
    if Input.IsMouseDown(Mouse.Left) then
        Log.Info("Left mouse held")
    end
    local mx, my = Input.GetMousePos()
    -- (silent unless you want to log every frame)
    -- Log.Info(string.format("mouse %.0f, %.0f", mx, my))

    -- ---- Pulsing light on this entity (if it has one) ---------------------
    if Engine.HasComponent(GetEntityID(), "Light") then
        local light = Engine.GetLight(GetEntityID())
        if light then
            light.intensity = 1.0 + math.sin(Time.elapsed * 3.0) * 0.5
        end
    end

    -- ---- Toggle-animation demo --------------------------------------------
    local anim = Engine.GetAnimation(GetEntityID())
    if anim and Input.IsKeyDown(Keys.Space) then
        anim.paused = not anim.paused
    end

    -- ---- Zoom camera by ID (set myCamera above to the Main Camera id) -----
    -- if myCamera ~= 0 then
    --     local cam = Engine.GetCamera(myCamera)
    --     if cam then
    --         cam.fov = 1.047 + math.sin(Time.elapsed) * 0.3
    --     end
    -- end
end
