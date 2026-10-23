/*
 * mod-dot-infinite
 *
 * Rend les DoT lancés par un joueur permanents sur leur cible :
 * ils durent jusqu'à un dispel, la mort de la cible ou un reset/evade.
 */

#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "Unit.h"

#include <string>
#include <unordered_set>

namespace
{
    bool   g_enable        = true;
    bool   g_affectPlayers = false;
    bool   g_includePets   = false;
    std::unordered_set<uint32> g_excludedSpells;

    void LoadConfig()
    {
        g_enable        = sConfigMgr->GetOption<bool>("DotInfinite.Enable", true);
        g_affectPlayers = sConfigMgr->GetOption<bool>("DotInfinite.AffectPlayers", false);
        g_includePets   = sConfigMgr->GetOption<bool>("DotInfinite.IncludePets", false);

        g_excludedSpells.clear();
        std::string const list = sConfigMgr->GetOption<std::string>("DotInfinite.ExcludedSpells", "");
        for (std::string_view token : Acore::Tokenize(list, ',', false))
            if (Optional<uint32> id = Acore::StringTo<uint32>(Acore::String::Trim(std::string(token))))
                g_excludedSpells.insert(*id);
    }

    bool IsPeriodicDamageAura(SpellInfo const* info, Aura const* aura)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!aura->HasEffect(i))
                continue;

            switch (info->Effects[i].ApplyAuraName)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_LEECH:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }
}

class DotInfinite_World : public WorldScript
{
public:
    DotInfinite_World() : WorldScript("DotInfinite_World") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class DotInfinite_Unit : public UnitScript
{
public:
    DotInfinite_Unit() : UnitScript("DotInfinite_Unit") { }

    void OnAuraApply(Unit* target, Aura* aura) override
    {
        if (!g_enable || !target || !aura)
            return;

        Unit* caster = aura->GetCaster();
        if (!caster || caster == target)
            return;

        // Lanceur : le joueur (ou son pet / gardien si activé)
        Player* player = g_includePets ? caster->GetCharmerOrOwnerPlayerOrPlayerItself()
                                       : caster->ToPlayer();
        if (!player)
            return;

        // Par défaut, on épargne les cibles joueurs (équilibre PvP)
        if (!g_affectPlayers && target->GetTypeId() == TYPEID_PLAYER)
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (!info || info->IsPositive() || info->IsChanneled() || aura->IsPermanent())
            return;

        if (g_excludedSpells.count(info->Id))
            return;

        if (!IsPeriodicDamageAura(info, aura))
            return;

        aura->SetMaxDuration(-1);
        aura->SetDuration(-1);
    }
};

void AddDotInfiniteScripts()
{
    new DotInfinite_World();
    new DotInfinite_Unit();
}
