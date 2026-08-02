/*
 * mod-spell-autotarget - commande joueur
 *
 * Ajoute la commande ".groundcast <sort>" (nom ou ID), utilisable par
 * n'importe quel joueur (pas besoin de droits GM). Contrairement au clic
 * sur une icône de sort, une commande est traitée entièrement côté
 * serveur : il n'y a donc AUCUN réticule à cliquer, le sort est lancé
 * directement.
 *
 * Le sort est lancé exactement comme un cast normal (vérifie que le
 * joueur connaît le sort, la mana, le temps de recharge, la portée, la
 * ligne de vue...) : ce n'est pas un raccourci "gratuit" de type GM.
 *
 * - Si le sort cible normalement une unité (single-target), il est lancé
 *   sur la cible actuelle du joueur.
 * - Si le sort nécessite une destination au sol (ex: Blizzard), la
 *   destination est calculée automatiquement sur la position de la cible
 *   actuelle (ou sous les pieds du joueur si aucune cible n'est
 *   sélectionnée).
 *
 * Exemple : ".groundcast Blizzard" ou ".groundcast 42208"
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Util.h"
#include "mod_spell_autotarget_shared.h"

using namespace Acore::ChatCommands;

namespace
{
    bool g_CommandEnabled = true;

    std::string_view Trim(std::string_view sv)
    {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
            sv.remove_prefix(1);
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
            sv.remove_suffix(1);
        return sv;
    }

    // Résout un sort connu du joueur à partir soit d'un ID numérique, soit
    // d'un nom tapé en texte brut (recherche insensible à la casse, dans
    // toutes les langues chargées par le serveur). Ne considère que les
    // sorts réellement appris ET actifs (le rang le plus élevé connu),
    // pour éviter d'ambiguïté entre plusieurs rangs partageant le même nom.
    SpellInfo const* ResolveKnownSpell(Player* player, std::string_view arg)
    {
        arg = Trim(arg);
        if (arg.empty())
            return nullptr;

        if (Optional<uint32> id = Acore::StringTo<uint32>(arg))
            return sSpellMgr->GetSpellInfo(*id);

        for (auto const& [spellId, playerSpell] : player->GetSpellMap())
        {
            if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
                continue;

            SpellInfo const* candidate = sSpellMgr->GetSpellInfo(spellId);
            if (!candidate)
                continue;

            for (uint8 locale = 0; locale < TOTAL_LOCALES; ++locale)
            {
                char const* name = candidate->SpellName[locale];
                if (name && *name && StringEqualI(name, arg))
                    return candidate;
            }
        }

        return nullptr;
    }
}

class SpellAutoTargetCommandWorldScript : public WorldScript
{
public:
    SpellAutoTargetCommandWorldScript() : WorldScript("SpellAutoTargetCommandWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_CommandEnabled = sConfigMgr->GetOption<bool>("SpellAutoTarget.EnableCommand", true);
    }
};

class spell_autotarget_commandscript : public CommandScript
{
public:
    spell_autotarget_commandscript() : CommandScript("spell_autotarget_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "groundcast", HandleGroundCastCommand, SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

    static bool HandleGroundCastCommand(ChatHandler* handler, Tail spellArg)
    {
        if (!g_CommandEnabled)
        {
            handler->SendSysMessage("Cette commande est désactivée sur ce serveur.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        SpellInfo const* spell = ResolveKnownSpell(player, spellArg);

        if (!spell)
        {
            handler->SendSysMessage("Sort introuvable, ou vous ne connaissez pas ce sort.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (spell->IsPassive())
        {
            handler->SendSysMessage("Ce sort est passif, il ne peut pas être lancé directement.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player->HasSpell(spell->Id))
        {
            handler->SendSysMessage("Vous ne connaissez pas ce sort.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Unit* target = player->GetVictim();
        if (!target)
            target = player->GetSelectedUnit();

        SpellCastResult result;

        if (ModSpellAutoTarget::HasGroundClickDestination(spell))
        {
            // Sort à ciblage au sol : destination = position de la cible
            // actuelle, ou sous les pieds du joueur si aucune cible.
            Position const pos = target ? target->GetPosition() : player->GetPosition();
            result = player->CastSpell(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), spell->Id, false);
        }
        else
        {
            if (!target)
            {
                handler->SendSysMessage("Vous devez sélectionner une cible.");
                handler->SetSentErrorMessage(true);
                return false;
            }

            result = player->CastSpell(target, spell->Id, false);
        }

        if (result != SPELL_CAST_OK)
        {
            // handler->PSendSysMessage("Impossible de lancer ce sort (code {}).", uint32(result));
            handler->SetSentErrorMessage(true);
            return false;
        }

        return true;
    }
};

void AddModSpellAutoTargetCommandScripts()
{
    new SpellAutoTargetCommandWorldScript();
    new spell_autotarget_commandscript();
}
