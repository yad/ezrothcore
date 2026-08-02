/*
 * mod-spell-autotarget
 *
 * Pour AzerothCore (WoW 3.3.5).
 *
 * Quand un joueur (ou un gardien : pet, familier de chasseur, totem...)
 * lance un sort dont l'effet nécessite normalement de cliquer au sol
 * (TARGET_DEST_DEST, ex: Blizzard), ce module repositionne automatiquement
 * la destination du sort sur la position de la cible actuellement
 * sélectionnée/attaquée, avant que le serveur ne calcule les cibles
 * touchées par l'effet.
 *
 * Limite technique importante : le client WoW affiche quand même le
 * réticule de ciblage au sol et attend un clic avant d'envoyer le paquet
 * de lancement (ça, c'est purement côté client, aucun module serveur ne
 * peut le supprimer). Ce module ne dispense donc pas de cliquer une fois
 * quelque part au sol - mais l'endroit exact du clic n'a plus d'importance :
 * l'effet atterrira toujours sur la cible actuelle du joueur.
 *
 * Pour un vrai "zéro clic", il existe une astuce 100% côté client, sans
 * mod ni add-on, avec une macro standard :
 *   #showtooltip
 *   /cast [target=target,exists] NomDuSort
 * Ce module et la macro sont complémentaires.
 */

#include "Config.h"
#include "EventProcessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "Unit.h"
#include "mod_spell_autotarget_shared.h"

#include <chrono>
#include <unordered_set>

using namespace std::chrono_literals;

namespace
{
    bool g_Enabled = true;
    bool g_IncludeGuardians = true;
    std::unordered_set<uint32> g_ExcludedSpells;

    bool g_RepeatCastEnabled = false;
    uint32 g_RepeatCastCount = 3; // nombre total de lancers (1 payant + (N-1) gratuits)
    std::unordered_set<uint32> g_RepeatCastSpells;

    // Reproduit exactement le calcul que fait le moteur au moment du cast
    // (Spell::handle_immediate) pour la durée réelle de canalisation :
    // durée de base -> mods de durée (talents) -> hâte, si applicable au
    // sort. C'est ce qui permet au délai entre deux relances de coller à
    // la vraie durée observée par le joueur (buffs de hâte inclus), au
    // lieu de la durée brute du sort.
    int32 CalcActualChannelDuration(Unit* caster, SpellInfo const* spellInfo)
    {
        int32 duration = spellInfo->GetDuration();
        if (duration <= 0)
            return duration;

        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_DURATION, duration);

        if (caster->HasAuraTypeWithAffectMask(SPELL_AURA_PERIODIC_HASTE, spellInfo) ||
            spellInfo->HasAttribute(SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC))
        {
            duration = int32(duration * caster->GetFloatValue(UNIT_MOD_CAST_SPEED));
        }

        return duration;
    }

    // Relance gratuitement (sans mana, mais avec canalisation/son normaux -
    // pas de bypass complet façon "triggered") le même sort sur la même
    // logique de ciblage que le cast d'origine (cible actuelle, ou position
    // de la cible actuelle pour un sort à ciblage au sol).
    void DoFreeRecast(Unit* caster, uint32 spellId)
    {
        if (!caster || !caster->IsInWorld() || !caster->IsAlive())
            return;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return;

        Unit* target = caster->GetVictim();
        if (!target && caster->IsPlayer())
            target = caster->ToPlayer()->GetSelectedUnit();

        SpellCastTargets targets;

        if (ModSpellAutoTarget::HasGroundClickDestination(spellInfo))
        {
            Position const pos = target ? target->GetPosition() : caster->GetPosition();
            targets.SetDst(pos);
        }
        else if (target)
        {
            targets.SetUnitTarget(target);
        }
        else
        {
            return; // pas de cible : on ne relance pas
        }

        // TRIGGERED_IGNORE_POWER_AND_REAGENT_COST seul (pas TRIGGERED_FULL_MASK)
        // pour garder un cast "normal" : temps de canalisation, son et
        // vérifications de portée/ligne de vue intacts - seul le coût en
        // mana/composant est ignoré.
        caster->CastSpell(targets, spellInfo, nullptr, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
    }
}

class SpellAutoTargetWorldScript : public WorldScript
{
public:
    SpellAutoTargetWorldScript() : WorldScript("SpellAutoTargetWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_Enabled = sConfigMgr->GetOption<bool>("SpellAutoTarget.Enable", true);
        g_IncludeGuardians = sConfigMgr->GetOption<bool>("SpellAutoTarget.IncludeGuardians", true);

        g_ExcludedSpells = ModSpellAutoTarget::ParseSpellIdList(
            sConfigMgr->GetOption<std::string>("SpellAutoTarget.ExcludedSpells", ""));

        g_RepeatCastEnabled = sConfigMgr->GetOption<bool>("SpellAutoTarget.RepeatCast.Enable", false);
        g_RepeatCastCount = sConfigMgr->GetOption<uint32>("SpellAutoTarget.RepeatCast.Count", 3);

        g_RepeatCastSpells = ModSpellAutoTarget::ParseSpellIdList(
            sConfigMgr->GetOption<std::string>("SpellAutoTarget.RepeatCast.Spells", ""));
    }
};

// AllSpellScript (alias historique : SpellSC) s'applique à TOUS les sorts,
// lancés par n'importe quel Unit (joueur, pet, totem, créature...), pas
// seulement par les joueurs.
class spell_autotarget_all : public AllSpellScript
{
public:
    spell_autotarget_all() : AllSpellScript("spell_autotarget_all", { ALLSPELLHOOK_ON_PREPARE }) { }

    // OnSpellPrepare est appelé juste après l'initialisation des cibles
    // explicites (position cliquée par le client incluse) mais AVANT la
    // résolution complète des cibles du sort (SelectSpellTargets) : c'est
    // le bon moment pour remplacer la destination.
    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        if (!g_Enabled || !spell || !caster || !spellInfo)
            return;

        bool const isGuardianCaster = caster->IsPet() || caster->IsGuardian() || caster->IsTotem();
        if (!caster->IsPlayer() && !(g_IncludeGuardians && isGuardianCaster))
            return;

        if (g_ExcludedSpells.count(spellInfo->Id))
            return;

        if (!ModSpellAutoTarget::HasGroundClickDestination(spellInfo))
            return;

        // Cible actuelle : celle qu'on attaque, ou à défaut la cible
        // sélectionnée par le joueur.
        Unit* target = caster->GetVictim();
        if (!target && caster->IsPlayer())
            target = caster->ToPlayer()->GetSelectedUnit();

        if (!target || target == caster)
            return;

        float const maxRange = spellInfo->GetMaxRange(false, caster, spell);
        if (maxRange > 0.0f && caster->GetDistance(target) > maxRange)
            return; // hors de portée : on laisse le comportement normal

        spell->m_targets.SetDst(*target);
    }
};

// Programme (N-1) relances gratuites (sans mana) après un cast payant d'un
// sort listé en config. Chaque relance est un cast complet et normal
// (temps de canalisation, son et animation d'origine intacts) : pas de
// désynchronisation visuelle côté client, juste plusieurs casts enchaînés.
class spell_autotarget_repeat_cast : public AllSpellScript
{
public:
    spell_autotarget_repeat_cast() : AllSpellScript("spell_autotarget_repeat_cast", { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!g_RepeatCastEnabled || !spell || !caster || !spellInfo)
            return;

        // Ne jamais réagir à un cast déjà déclenché automatiquement (sinon
        // nos propres relances gratuites relanceraient elles-mêmes une
        // chaîne de relances à l'infini).
        if (spell->IsTriggered())
            return;

        if (g_RepeatCastCount < 2 || !g_RepeatCastSpells.count(spellInfo->Id))
            return;

        int32 delayMs = CalcActualChannelDuration(caster, spellInfo);
        if (delayMs <= 0)
            delayMs = int32(spellInfo->CalcCastTime(caster, spell)); // sort non canalisé : le temps de lancement (déjà hâté) sert de délai
        if (delayMs <= 0)
            return;

        uint32 const spellId = spellInfo->Id;

        for (uint32 i = 1; i < g_RepeatCastCount; ++i)
        {
            caster->m_Events.AddEventAtOffset([caster, spellId]()
            {
                DoFreeRecast(caster, spellId);
            }, std::chrono::milliseconds(delayMs * i));
        }
    }
};

void AddModSpellAutoTargetScripts()
{
    new SpellAutoTargetWorldScript();
    new spell_autotarget_all();
    new spell_autotarget_repeat_cast();
}
