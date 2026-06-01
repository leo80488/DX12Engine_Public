-- weapons.lua — Config script (Lua-as-data, doc §4 / §11.5).
--
-- Pure data. No callbacks. Loaded explicitly via:
--     local weapons = Engine.LoadConfig("asset/scripts/config/weapons.lua")
--     local sword   = weapons.sword
--
-- Designers can edit numbers here without touching engine code; hot reload
-- by re-calling Engine.LoadConfig (caller's responsibility).

return {
    sword = {
        name        = "Iron Sword",
        damage      = 25,
        crit_rate   = 0.10,
        crit_mult   = 1.5,
        attack_type = "neutral",
        cooldown    = 0.4,
    },
    fireball = {
        name        = "Fireball",
        damage      = 60,
        crit_rate   = 0.05,
        crit_mult   = 2.0,
        attack_type = "fire",
        cooldown    = 1.2,
        mana_cost   = 30,
    },
    bow = {
        name        = "Hunter's Bow",
        damage      = 18,
        crit_rate   = 0.20,
        crit_mult   = 2.0,
        attack_type = "neutral",
        cooldown    = 0.6,
        range       = 25.0,
    },
}
