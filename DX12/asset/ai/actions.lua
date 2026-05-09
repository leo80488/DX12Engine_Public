-- Shared Lua actions / conditions for the BT runtime.
-- This file is loaded once via ScriptSystem global-script registration
-- (see App.cpp:m_scriptSystem.AddGlobalScript). New actions go here;
-- *.bt.lua trees reference them by string name.

------------------------------------------------------------
-- Conditions
------------------------------------------------------------

-- Pure check: returns Success / Failure. Reads a numeric blackboard
-- value and tests against a threshold passed via params.
Conditions.HealthBelow = function(ctx, params)
    local hp = ctx:GetBB("Health") or 100
    local th = params.threshold or 30
    if hp < th then return BT.Success end
    return BT.Failure
end

-- Distance check using the engine's GlobalTransform via DistanceTo helper.
-- params.target  — entity id stored on the blackboard under that key
-- params.range   — float; pass succeeds if distance < range
Conditions.InRange = function(ctx, params)
    local targetId = ctx:GetBB(params.target or "TargetEntity")
    if not targetId or targetId == 0 then return BT.Failure end
    local d = ctx:DistanceTo(targetId)
    return d < (params.range or 5.0) and BT.Success or BT.Failure
end

------------------------------------------------------------
-- Actions
------------------------------------------------------------

-- Stage a destination on the blackboard for MovementSystem to consume.
-- params.target  — blackboard key holding the target entity id
-- params.speed   — optional movement speed override
Actions.MoveToTarget = function(ctx, params)
    local targetId = ctx:GetBB(params.target or "TargetEntity")
    if not targetId or targetId == 0 then return BT.Failure end

    local p = ctx:GetEntityPosition(targetId)
    ctx:SetBB("MoveDestination", { x = p.x, y = p.y, z = p.z })
    if params.speed then ctx:SetBB("MoveSpeed", params.speed) end

    if ctx:DistanceTo(targetId) < (params.arriveDistance or 1.0) then
        return BT.Success
    end
    return BT.Running
end

-- Designer-friendly debug action — prints the entity id and a label.
-- Useful for verifying tree wiring before any real actions exist.
Actions.LogTick = function(ctx, params)
    print(string.format("[AI] entity=%d msg=%s",
        ctx:Entity(), tostring(params.message or "tick")))
    return BT.Success
end

-- Idle / patrol stub: returns Running indefinitely so a Selector falls
-- back to it when no other branch wants the tick.
Actions.Idle = function(ctx, params)
    return BT.Running
end
