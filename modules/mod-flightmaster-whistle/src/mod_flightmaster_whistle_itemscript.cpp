#include "ScriptMgr.h"
#include "SpellScript.h"
#include "Player.h"
#include "Map.h"
#include "flightmaster_whistle.h"

enum FlightmasterWhistleSpells
{
    SPELL_HEARTHSTONE = 8690,
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
            return SPELL_CAST_OK; // laisse la vraie Pierre de Foyer tranquille

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

        return SPELL_CAST_OK;
    }

    void HandleTeleportEffect(SpellEffIndex effIndex)
    {
        if (!IsWhistleCast())
            return; // comportement par défaut de la Pierre de Foyer inchangé

        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        PreventHitDefaultEffect(effIndex); // bloque le "retour au point de rappel"
        sFlightmasterWhistle->TeleportToNearestFlightmaster(player);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_flightmaster_whistle_cast::CheckCast);
        OnEffectHit += SpellEffectFn(
            spell_flightmaster_whistle_cast::HandleTeleportEffect,
            EFFECT_0,
            SPELL_EFFECT_TELEPORT_UNITS // c'est l'effet réel du sort 8690, pas SCRIPT_EFFECT
        );
    }
};

void AddSC_mod_flightmaster_whistle_itemscript()
{
    RegisterSpellScript(spell_flightmaster_whistle_cast);
}