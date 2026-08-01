#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Player.h"
#include "Item.h"
#include "flightmaster_whistle.h"


enum FlightmasterWhistleSpells
{
    SPELL_FLIGHTMASTER_WHISTLE_CAST = 110001
};


class spell_flightmaster_whistle_cast : public SpellScript
{
    PrepareSpellScript(spell_flightmaster_whistle_cast);

    void HandleScriptEffect(SpellEffIndex)
    {
        Player* player = GetCaster()->ToPlayer();

        if (!player)
            return;

        sFlightmasterWhistle->TeleportToNearestFlightmaster(player);
    }


    void Register() override
    {
        OnEffectHit += SpellEffectFn(
            spell_flightmaster_whistle_cast::HandleScriptEffect,
            EFFECT_0,
            SPELL_EFFECT_SCRIPT_EFFECT
        );
    }
};



class item_flightmaster_whistle : public ItemScript
{
public:

    item_flightmaster_whistle()
        : ItemScript("flightmaster_whistle")
    {
    }


    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        player->CastSpell(
            player,
            SPELL_FLIGHTMASTER_WHISTLE_CAST,
            false,
            item
        );

        return true;
    }
};



void AddSC_mod_flightmaster_whistle_itemscript()
{
    RegisterSpellScript(spell_flightmaster_whistle_cast);

    new item_flightmaster_whistle();
}