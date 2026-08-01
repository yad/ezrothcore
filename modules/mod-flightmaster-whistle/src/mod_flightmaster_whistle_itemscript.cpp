/*
 * Credits: silviu20092
 */

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "flightmaster_whistle.h"

enum FlightmasterWhistleSpells
{
    SPELL_FLIGHTMASTER_WHISTLE_CAST = 110001
};

class spell_flightmaster_whistle_cast : public SpellScript
{
    PrepareSpellScript(spell_flightmaster_whistle_cast);

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        if (Player* player = GetHitPlayer())
            sFlightmasterWhistle->TeleportToNearestFlightmaster(player);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_flightmaster_whistle_cast::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

class flightmaster_whistle : public ItemScript
{
public:
    flightmaster_whistle() : ItemScript("flightmaster_whistle") {}

    bool OnUse(Player* player, Item* /*item*/, SpellCastTargets const& /*targets*/) override
    {
        if (player)
        {
            player->CastSpell(player, SPELL_FLIGHTMASTER_WHISTLE_CAST, false);
        }
        return true;
    }
};

void AddSC_mod_flightmaster_whistle_itemscript()
{
    RegisterSpellScript(spell_flightmaster_whistle_cast);
    new flightmaster_whistle();
}