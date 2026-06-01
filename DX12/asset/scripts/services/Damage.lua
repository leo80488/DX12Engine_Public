-- Damage.lua — Service script (stateless module, doc §3.4).
--
-- Pure functions; no callbacks, no per-frame tick. Other scripts pull this
-- via Engine.GetService("Damage") and call its functions:
--
--     local Damage = Engine.GetService("Damage")
--     local result = Damage.Calculate(attacker, target, 100, "fire")

local Damage = {}

-- Element multiplier table. Row = attacker, column = target.
local ELEMENT_MULT = {
    fire    = { fire = 0.5, ice = 2.0, water = 0.5, earth = 1.0, neutral = 1.0 },
    ice     = { fire = 0.5, ice = 0.5, water = 1.0, earth = 1.0, neutral = 1.0 },
    water   = { fire = 2.0, ice = 1.0, water = 0.5, earth = 0.5, neutral = 1.0 },
    earth   = { fire = 1.0, ice = 1.0, water = 2.0, earth = 0.5, neutral = 1.0 },
    neutral = { fire = 1.0, ice = 1.0, water = 1.0, earth = 1.0, neutral = 1.0 },
}

function Damage.GetElementMultiplier(damageType, targetElement)
    local row = ELEMENT_MULT[damageType] or ELEMENT_MULT.neutral
    return row[targetElement] or 1.0
end

-- attacker / target are plain Lua tables (your gameplay layer fills these in).
-- Expected fields:
--   attacker.attack_power : number
--   attacker.crit_rate    : number  (0..1)
--   attacker.crit_mult    : number  (e.g. 1.5)
--   target.GetDefense(damageType) : returns defense vs that type
--   target.element         : string
function Damage.Calculate(attacker, target, baseDamage, damageType)
    local atk  = attacker.attack_power or 0
    local def  = (target.GetDefense and target:GetDefense(damageType)) or 0
    local crit = math.random() < (attacker.crit_rate or 0)

    local final = (baseDamage + atk) * (100 / (100 + def))
    if crit then final = final * (attacker.crit_mult or 1.5) end

    final = final * Damage.GetElementMultiplier(damageType, target.element or "neutral")

    return { amount = math.floor(final + 0.5), is_crit = crit }
end

return Damage
