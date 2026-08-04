#include "ScriptMgr.h"
#include "SpellScript.h"
#include "Player.h"
#include "Item.h"
#include "Map.h"
#include "flightmaster_whistle.h"

enum FlightmasterWhistleSpells
{
    SPELL_COPY_OF_HEARTHSTONE = 54401,
    ITEM_FLIGHTMASTER_WHISTLE = 70000
};

class spell_flightmaster_whistle_cast : public SpellScript
{
    PrepareSpellScript(spell_flightmaster_whistle_cast);

    bool IsWhistleCast()
    {
        Item* castItem = GetCastItem();
        return castItem && castItem->GetEntry() == ITEM_FLIGHTMASTER_WHISTLE;
    }

    SpellCastResult CheckCast()
    {
        if (!IsWhistleCast())
            return SPELL_CAST_OK;

        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_DONT_REPORT;

        if (!sFlightmasterWhistle->GetEnabled())
            return SPELL_FAILED_NOT_HERE;

        if (player->GetLevel() < sFlightmasterWhistle->GetMinPlayerLevel())
            return SPELL_FAILED_LEVEL_REQUIREMENT;

        if (!player->IsAlive())
            return SPELL_FAILED_CASTER_DEAD;

        if (player->IsInCombat())
            return SPELL_FAILED_AFFECTING_COMBAT;

        if (player->InArena())
            return SPELL_FAILED_NOT_IN_ARENA;

        Map* map = player->GetMap();
        if (map && map->Instanceable())
            return SPELL_FAILED_NOT_HERE;

        if (!sFlightmasterWhistle->HasNearestFlightmaster(player))
            return SPELL_FAILED_NOT_HERE;

        return SPELL_CAST_OK;
    }

    // EFFECT_0 = SPELL_EFFECT_TELEPORT_UNITS -> bloque le comportement natif (retour auberge)
    void BlockNativeTeleport(SpellEffIndex effIndex)
    {
        if (IsWhistleCast())
            PreventHitDefaultEffect(effIndex);
    }

    // EFFECT_1 = SPELL_EFFECT_SCRIPT_EFFECT -> notre logique custom
    void HandleWhistleEffect(SpellEffIndex /*effIndex*/)
    {
        if (!IsWhistleCast())
            return;

        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        sFlightmasterWhistle->TeleportToNearestFlightmaster(player);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_flightmaster_whistle_cast::CheckCast);

        OnEffectHitTarget += SpellEffectFn(
            spell_flightmaster_whistle_cast::BlockNativeTeleport,
            EFFECT_0,
            SPELL_EFFECT_TELEPORT_UNITS
        );

        OnEffectHit += SpellEffectFn(
            spell_flightmaster_whistle_cast::HandleWhistleEffect,
            EFFECT_1,
            SPELL_EFFECT_SCRIPT_EFFECT
        );
    }
};

void AddSC_mod_flightmaster_whistle_itemscript()
{
    RegisterSpellScript(spell_flightmaster_whistle_cast);
}
