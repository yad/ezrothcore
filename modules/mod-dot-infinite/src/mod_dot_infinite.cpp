/*
 * mod-dot-infinite
 *
 * 1) Les DoT lancés par un joueur voient leur durée portée à une valeur
 *    configurable (120 s par défaut, 0 = permanent jusqu'au dispel / mort).
 * 2) Les DoT posés sont "mémorisés" : tout ennemi qui entre en combat contre le
 *    joueur (ou son groupe) reçoit les mêmes DoT.
 */

#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Unit.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // --- Configuration -------------------------------------------------------
    bool     g_enable         = true;
    bool     g_affectPlayers  = false;
    bool     g_includePets    = false;
    int32    g_durationMs     = 120 * IN_MILLISECONDS; // <= 0 => permanent (-1)
    bool     g_spreadEnable   = true;
    float    g_spreadRange    = 60.0f;                 // yards, autour du lanceur
    std::unordered_set<uint32> g_excludedSpells;

    // --- Suivi des DoT posés (pour la propagation) ----------------------------
    struct TrackedDot
    {
        Clock::time_point expires;
    };

    // GUID du lanceur -> (spellId -> expiration)
    std::mutex g_trackMutex;
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, TrackedDot>> g_tracked;

    // Évite que les auras posées par la propagation relancent le suivi
    thread_local bool t_spreading = false;

    void LoadConfig()
    {
        g_enable        = sConfigMgr->GetOption<bool>("DotInfinite.Enable", true);
        g_affectPlayers = sConfigMgr->GetOption<bool>("DotInfinite.AffectPlayers", false);
        g_includePets   = sConfigMgr->GetOption<bool>("DotInfinite.IncludePets", false);

        uint32 seconds  = sConfigMgr->GetOption<uint32>("DotInfinite.Duration", 120);
        // 0 = permanent ; on borne pour ne pas dépasser int32 en millisecondes
        g_durationMs    = seconds == 0 ? -1 : static_cast<int32>(std::min<uint32>(seconds, 2000000u) * IN_MILLISECONDS);

        g_spreadEnable  = sConfigMgr->GetOption<bool>("DotInfinite.Spread.Enable", true);
        g_spreadRange   = sConfigMgr->GetOption<float>("DotInfinite.Spread.Range", 60.0f);

        g_excludedSpells.clear();
        std::istringstream stream(sConfigMgr->GetOption<std::string>("DotInfinite.ExcludedSpells", ""));
        std::string token;
        while (std::getline(stream, token, ','))
        {
            try
            {
                if (!token.empty())
                    g_excludedSpells.insert(static_cast<uint32>(std::stoul(token)));
            }
            catch (...) { }
        }
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

    void Track(ObjectGuid casterGuid, uint32 spellId, int32 durationMs)
    {
        // Permanent : on garde l'entrée jusqu'à la fin du combat
        auto expires = durationMs > 0 ? Clock::now() + std::chrono::milliseconds(durationMs)
                                      : Clock::time_point::max();

        std::lock_guard<std::mutex> lock(g_trackMutex);
        g_tracked[casterGuid][spellId] = { expires };
    }

    void Untrack(ObjectGuid casterGuid)
    {
        std::lock_guard<std::mutex> lock(g_trackMutex);
        g_tracked.erase(casterGuid);
    }
}

class DotInfinite_World : public WorldScript
{
public:
    DotInfinite_World() : WorldScript("DotInfinite_World", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class DotInfinite_Unit : public UnitScript
{
public:
    DotInfinite_Unit() : UnitScript("DotInfinite_Unit", true,
        { UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_UNIT_ENTER_COMBAT }) { }

    // Allonge la durée du DoT et le mémorise pour la propagation
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

        // Jamais plus court que la durée naturelle du sort
        int32 duration = g_durationMs;
        if (duration > 0)
            duration = std::max(duration, aura->GetMaxDuration());

        aura->SetMaxDuration(duration);
        aura->SetDuration(duration);

        if (g_spreadEnable && !t_spreading)
            Track(caster->GetGUID(), info->Id, duration);
    }

    // Propage les DoT mémorisés aux ennemis qui rejoignent le combat
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (!g_enable || !g_spreadEnable || !unit || !victim)
            return;

        Creature* enemy = unit->ToCreature();
        if (!enemy || !enemy->IsAlive() || enemy->IsControlledByPlayer())
            return;

        Player* victimPlayer = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!victimPlayer)
            return;

        struct Pending { ObjectGuid caster; uint32 spellId; };
        std::vector<Pending> pending;

        {
            std::lock_guard<std::mutex> lock(g_trackMutex);
            if (g_tracked.empty())
                return;

            Clock::time_point const now = Clock::now();

            for (auto casterIt = g_tracked.begin(); casterIt != g_tracked.end(); )
            {
                // Purge des DoT expirés
                auto& spells = casterIt->second;
                for (auto it = spells.begin(); it != spells.end(); )
                    it = (it->second.expires <= now) ? spells.erase(it) : std::next(it);

                if (spells.empty())
                {
                    casterIt = g_tracked.erase(casterIt);
                    continue;
                }

                Unit* caster = ObjectAccessor::GetUnit(*enemy, casterIt->first);
                if (caster && caster->IsInWorld() && caster->GetMap() == enemy->GetMap())
                {
                    Player* owner = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
                    bool const sameGroup = owner &&
                        (owner == victimPlayer || owner->IsInSameGroupWith(victimPlayer));

                    if (sameGroup && caster->IsAlive() && caster->IsHostileTo(enemy) &&
                        (g_spreadRange <= 0.0f || caster->IsWithinDist(enemy, g_spreadRange)))
                    {
                        for (auto const& [spellId, dot] : spells)
                            pending.push_back({ casterIt->first, spellId });
                    }
                }

                ++casterIt;
            }
        }

        // Application hors verrou : AddAura rappelle OnAuraApply
        t_spreading = true;
        for (Pending const& p : pending)
        {
            Unit* caster = ObjectAccessor::GetUnit(*enemy, p.caster);
            if (!caster || !enemy->IsAlive() || enemy->HasAura(p.spellId, caster->GetGUID()))
                continue;

            caster->AddAura(p.spellId, enemy);
        }
        t_spreading = false;
    }
};

class DotInfinite_Player : public PlayerScript
{
public:
    DotInfinite_Player() : PlayerScript("DotInfinite_Player",
        { PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT, PLAYERHOOK_ON_LOGOUT }) { }

    // Fin de combat : on oublie les DoT à propager
    void OnPlayerLeaveCombat(Player* player) override
    {
        if (player)
            Untrack(player->GetGUID());
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            Untrack(player->GetGUID());
    }
};

void AddDotInfiniteScripts()
{
    new DotInfinite_World();
    new DotInfinite_Unit();
    new DotInfinite_Player();
}
