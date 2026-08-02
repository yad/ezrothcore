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
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "Unit.h"
#include "mod_spell_autotarget_shared.h"

#include <unordered_set>

namespace
{
    bool g_Enabled = true;
    bool g_IncludeGuardians = true;
    std::unordered_set<uint32> g_ExcludedSpells;
}

class SpellAutoTargetWorldScript : public WorldScript
{
public:
    SpellAutoTargetWorldScript() : WorldScript("SpellAutoTargetWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_Enabled = sConfigMgr->GetOption<bool>("SpellAutoTarget.Enable", true);
        g_IncludeGuardians = sConfigMgr->GetOption<bool>("SpellAutoTarget.IncludeGuardians", true);

        g_ExcludedSpells.clear();
        std::string excluded = sConfigMgr->GetOption<std::string>("SpellAutoTarget.ExcludedSpells", "");
        for (std::string_view token : Acore::Tokenize(excluded, ',', false))
        {
            if (Optional<uint32> id = Acore::StringTo<uint32>(token))
                g_ExcludedSpells.insert(*id);
        }
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

void AddModSpellAutoTargetScripts()
{
    new SpellAutoTargetWorldScript();
    new spell_autotarget_all();
}
