/*
 * mod-dot-infinite
 *
 * 1) Deux familles de DoT, deux comportements :
 *    - À DISTANCE : tous les DoT lancés de loin (magie, malédictions, maladies,
 *               Moonfire, Serpent Sting...). Ils se propagent d'eux-mêmes à
 *               toute cible en combat contre le joueur ou son groupe : à la
 *               pose, à chaque tick, et à chaque ennemi qui rejoint le combat.
 *    - MÊLÉE  : saignements (toutes classes : guerrier, voleur, druide...) et
 *               poisons du voleur. Aucune auto-propagation. À chaque coup porté
 *               par le joueur, ses DoT de mêlée sont (ré)appliqués à tout le
 *               pack d'ennemis engagés qui est au corps à corps.
 *
 * Aucune liste mémorisée : les DoT à propager sont toujours lus sur les auras
 * réellement présentes sur les ennemis. Un DoT dispel ou une cible morte
 * n'est donc plus propagé.
 */

#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "GroupReference.h"
#include "Pet.h"
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

    enum class DotMode : uint8
    {
        Ranged, // DoT lancé à distance : se propage de lui-même à toute cible en combat
        Melee   // DoT de mêlée : appliqué au pack d'ennemis au corps à corps à chaque coup
    };

    // --- Configuration -------------------------------------------------------
    bool   g_enable          = true;
    bool   g_includePets     = false;
    bool   g_rangedEnable    = true;                  // DoT à distance
    float  g_rangedRange     = 60.0f;                 // yards entre créatures en combat (0 = illimité)

    bool   g_meleeEnable     = true;                  // DoT de mêlée
    float  g_meleeRange      = 2.0f;                  // yards en plus de la portée de mêlée
    uint32 g_meleeIntervalMs = 250;                   // intervalle min. entre deux évaluations
    std::unordered_set<uint32> g_excludedSpells;

    // --- État ----------------------------------------------------------------

    // Empêche les auras posées par la propagation de se propager à leur tour
    thread_local bool t_spreading = false;

    struct SpreadGuard
    {
        bool previous;
        SpreadGuard() : previous(t_spreading) { t_spreading = true; }
        ~SpreadGuard() { t_spreading = previous; }
    };

    // Un coup de mêlée (attaque normale) est "armé" au calcul des dégâts, puis
    // validé quand les dégâts sont réellement infligés (un coup raté n'en inflige pas).
    struct SwingMark
    {
        bool armed = false;
        ObjectGuid attacker;
        ObjectGuid victim;
        Clock::time_point at;
    };
    thread_local SwingMark t_swing;

    std::mutex g_meleeMutex;
    std::unordered_map<ObjectGuid, Clock::time_point> g_lastMeleeEval;

    // --- Configuration : chargement ---------------------------------------------
    template <typename T>
    void ParseIdList(std::string const& text, std::unordered_set<T>& out)
    {
        out.clear();
        std::istringstream stream(text);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            try
            {
                if (!token.empty())
                    out.insert(static_cast<T>(std::stoul(token)));
            }
            catch (...) { }
        }
    }

    void LoadConfig()
    {
        g_enable        = sConfigMgr->GetOption<bool>("DotInfinite.Enable", true);
        g_includePets   = sConfigMgr->GetOption<bool>("DotInfinite.IncludePets", false);

        g_rangedEnable = sConfigMgr->GetOption<bool>("DotInfinite.Ranged.Enable", true);
        g_rangedRange  = sConfigMgr->GetOption<float>("DotInfinite.Ranged.Range", 60.0f);

        g_meleeEnable     = sConfigMgr->GetOption<bool>("DotInfinite.Melee.Enable", true);
        g_meleeRange      = sConfigMgr->GetOption<float>("DotInfinite.Melee.Range", 2.0f);
        g_meleeIntervalMs = sConfigMgr->GetOption<uint32>("DotInfinite.Melee.Interval", 250);

        ParseIdList(sConfigMgr->GetOption<std::string>("DotInfinite.ExcludedSpells", ""), g_excludedSpells);
    }

    // --- Classification --------------------------------------------------------

    // Joueur propriétaire du DoT (le lanceur, ou le maître de son pet si activé)
    Player* GetDotOwner(Unit* caster)
    {
        if (!caster)
            return nullptr;

        return g_includePets ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : caster->ToPlayer();
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

    // DoT concerné par le module (indépendamment du mode)
    bool IsEligibleDot(SpellInfo const* info, Aura const* aura)
    {
        if (!info || info->IsPositive() || info->IsChanneled() || info->IsSingleTarget())
            return false;

        // Zones au sol (Consécration, Death and Decay, pièges...) : l'aura appartient à un
        // objet dynamique dont la durée de vie dépend de la sienne. On n'y touche pas.
        if (aura->GetType() == DYNOBJ_AURA_TYPE || info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA))
            return false;

        if (g_excludedSpells.count(info->Id))
            return false;

        return IsPeriodicDamageAura(info, aura);
    }

    // Classification par le sort lui-même (indépendante de la classe du lanceur) :
    //  - saignement (mécanique BLEED) : Rend, Rupture, Garrote, Deep Wounds, Rake, Rip, Lacerate...
    //  - poison de la famille de sorts du voleur : Deadly Poison
    // => MÊLÉE ; tout le reste (magie, malédictions, maladies, Moonfire...) => À DISTANCE
    DotMode Classify(Player const* /*owner*/, SpellInfo const* info)
    {
        bool const bleed = (info->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_BLEED)) != 0;
        bool const roguePoison = info->Dispel == DISPEL_POISON && info->SpellFamilyName == SPELLFAMILY_ROGUE;

        return (bleed || roguePoison) ? DotMode::Melee : DotMode::Ranged;
    }

    // --- Recherche des alliés et des ennemis engagés ----------------------------

    // Le joueur, son pet, et les membres de son groupe (et leurs pets) sur la même carte
    std::vector<Unit*> CollectAllies(Player* owner)
    {
        std::vector<Unit*> allies;
        std::unordered_set<Unit*> seen;

        auto add = [&](Unit* unit)
        {
            if (unit && unit->IsInWorld() && seen.insert(unit).second)
                allies.push_back(unit);
        };

        add(owner);
        add(owner->GetPet());

        if (Group* group = owner->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    if (member->GetMap() == owner->GetMap())
                    {
                        add(member);
                        add(member->GetPet());
                    }

        return allies;
    }

    // Créatures vivantes actuellement engagées en combat PvE avec l'un des alliés.
    // La référence de combat inclut aussi les créatures neutres devenues hostiles.
    std::vector<Creature*> CollectEngagedEnemies(std::vector<Unit*> const& allies)
    {
        std::vector<Creature*> enemies;
        std::unordered_set<Creature*> seen;

        for (Unit* ally : allies)
            for (auto const& pair : ally->GetCombatManager().GetPvECombatRefs())
            {
                Creature* enemy = pair.second->GetOther(ally)->ToCreature();
                if (!enemy || !enemy->IsAlive() || enemy->IsControlledByPlayer())
                    continue;

                if (!seen.insert(enemy).second)
                    continue;

                enemies.push_back(enemy);
            }

        return enemies;
    }

    bool InRangedRange(Unit const* source, Unit const* enemy)
    {
        return g_rangedRange <= 0.0f || source->IsWithinDist(enemy, g_rangedRange);
    }

    // --- Lecture des DoT présents sur les ennemis ---------------------------------

    using DotSources = std::unordered_map<uint32, uint8>; // spellId -> nombre de charges

    // DoT du lanceur, du mode demandé, actuellement présents sur les ennemis
    DotSources CollectSources(Unit* caster, Player* owner, std::vector<Creature*> const& enemies, DotMode mode)
    {
        DotSources sources;
        ObjectGuid const casterGuid = caster->GetGUID();

        for (Creature* enemy : enemies)
            for (auto const& entry : enemy->GetAppliedAuras())
            {
                Aura* aura = entry.second->GetBase();
                if (!aura || aura->GetCasterGUID() != casterGuid)
                    continue;

                SpellInfo const* info = aura->GetSpellInfo();
                if (!IsEligibleDot(info, aura) || Classify(owner, info) != mode)
                    continue;

                uint8& stacks = sources[info->Id];
                stacks = std::max<uint8>(stacks, aura->GetStackAmount());
            }

        return sources;
    }

    // --- Application ----------------------------------------------------------------

    // Pose le DoT sur l'ennemi avec le même nombre de charges, sans modifier
    // la durée d'une aura déjà présente.
    void ApplyDot(Unit* caster, uint32 spellId, uint8 stacks, Unit* enemy)
    {
        if (enemy->GetAura(spellId, caster->GetGUID()))
            return;

        if (Aura* aura = caster->AddAura(spellId, enemy))
            if (stacks > aura->GetStackAmount())
                aura->SetStackAmount(stacks);
    }

    // DoT à distance : propage un DoT à tous les ennemis engagés qui ne l'ont pas
    void SpreadAura(Unit* caster, Player* owner, Aura* aura, Unit const* source)
    {
        std::vector<Unit*> const allies = CollectAllies(owner);
        std::vector<Creature*> const enemies = CollectEngagedEnemies(allies);

        uint32 const spellId = aura->GetId();
        uint8 const stacks = aura->GetStackAmount();

        SpreadGuard guard;
        for (Creature* enemy : enemies)
        {
            if (enemy == source || !enemy->IsAlive() || !InRangedRange(source, enemy))
                continue;

            ApplyDot(caster, spellId, stacks, enemy);
        }
    }

    bool PassMeleeThrottle(ObjectGuid guid)
    {
        if (g_meleeIntervalMs == 0)
            return true;

        Clock::time_point const now = Clock::now();

        std::lock_guard<std::mutex> lock(g_meleeMutex);
        Clock::time_point& last = g_lastMeleeEval[guid];
        if (now - last < std::chrono::milliseconds(g_meleeIntervalMs))
            return false;

        last = now;
        return true;
    }

    // DoT de mêlée : un coup du joueur (ré)applique ses DoT de mêlée à toutes
    // les créatures engagées qui sont à portée de mêlée
    void HandleMeleeHit(Player* player)
    {
        if (!g_enable || !g_meleeEnable || !player->IsAlive() || !player->IsInWorld())
            return;

        if (!PassMeleeThrottle(player->GetGUID()))
            return;

        std::vector<Unit*> const allies = CollectAllies(player);
        std::vector<Creature*> const enemies = CollectEngagedEnemies(allies);
        if (enemies.empty())
            return;

        // Les DoT sont lus sur tous les ennemis engagés, où qu'ils soient
        DotSources const sources = CollectSources(player, player, enemies, DotMode::Melee);
        if (sources.empty())
            return;

        SpreadGuard guard;
        for (Creature* enemy : enemies)
        {
            if (!enemy->IsAlive() || !player->IsWithinMeleeRange(enemy, g_meleeRange))
                continue;

            for (auto const& [spellId, stacks] : sources)
                ApplyDot(player, spellId, stacks, enemy);
        }
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
        {
            UNITHOOK_ON_AURA_APPLY,
            UNITHOOK_ON_UNIT_ENTER_COMBAT,
            UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
            UNITHOOK_MODIFY_MELEE_DAMAGE,
            UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
            UNITHOOK_ON_DAMAGE
        }) { }

    // Propagation immédiate pour les DoT à distance
    void OnAuraApply(Unit* target, Aura* aura) override
    {
        if (!g_enable || !target || !aura)
            return;

        Unit* caster = aura->GetCaster();
        if (!caster || caster == target)
            return;

        Player* owner = GetDotOwner(caster);
        if (!owner)
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (!IsEligibleDot(info, aura))
            return;

        // DoT à distance : propagation immédiate aux ennemis déjà engagés
        if (g_rangedEnable && !t_spreading && target->IsCreature() && Classify(owner, info) == DotMode::Ranged)
            SpreadAura(caster, owner, aura, target);
    }

    // DoT à distance : à chaque tick, le DoT est propagé aux ennemis engagés qui ne l'ont pas
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!g_enable || !g_rangedEnable || !target || !attacker || !spellInfo || spellInfo->IsPositive())
            return;

        if (!target->IsCreature() || !attacker->IsInWorld())
            return;

        Player* owner = GetDotOwner(attacker);
        if (!owner || !owner->IsInWorld())
            return;

        Aura* aura = target->GetAura(spellInfo->Id, attacker->GetGUID());
        if (!aura || !IsEligibleDot(spellInfo, aura) || Classify(owner, spellInfo) != DotMode::Ranged)
            return;

        SpreadAura(attacker, owner, aura, target);
    }

    // DoT à distance : un ennemi qui rejoint le combat reçoit les DoT déjà présents sur les autres
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (!g_enable || !g_rangedEnable || !unit || !victim)
            return;

        Creature* enemy = unit->ToCreature();
        if (!enemy || !enemy->IsAlive() || enemy->IsControlledByPlayer())
            return;

        Player* owner = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!owner)
            return;

        std::vector<Unit*> const allies = CollectAllies(owner);
        std::vector<Creature*> const engaged = CollectEngagedEnemies(allies);
        if (engaged.empty())
            return;

        // On lit d'abord tout, on modifie ensuite. La portée part de chaque créature
        // qui porte déjà le DoT, pas du lanceur.
        struct Job
        {
            Unit* caster;
            Creature* source;
            DotSources sources;
        };
        std::vector<Job> jobs;

        for (Creature* source : engaged)
            for (Unit* ally : allies)
            {
                Player* allyOwner = GetDotOwner(ally);
                if (!allyOwner)
                    continue;

                DotSources sources = CollectSources(ally, allyOwner, { source }, DotMode::Ranged);
                if (!sources.empty())
                    jobs.push_back({ ally, source, std::move(sources) });
            }

        SpreadGuard guard;
        for (Job const& job : jobs)
        {
            if (!job.caster->IsAlive() || !job.source->IsAlive() || !InRangedRange(job.source, enemy))
                continue;

            for (auto const& [spellId, stacks] : job.sources)
                ApplyDot(job.caster, spellId, stacks, enemy);
        }
    }

    // DoT de mêlée, attaque normale : armée au calcul des dégâts...
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& /*damage*/) override
    {
        if (!g_enable || !g_meleeEnable || !target || !attacker || !attacker->IsPlayer())
            return;

        t_swing.armed = true;
        t_swing.attacker = attacker->GetGUID();
        t_swing.victim = target->GetGUID();
        t_swing.at = Clock::now();
    }

    // ... et validée quand des dégâts sont réellement infligés (coup non raté)
    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override
    {
        if (!t_swing.armed || !attacker || !victim)
            return;

        if (Clock::now() - t_swing.at > std::chrono::milliseconds(100))
        {
            t_swing.armed = false;
            return;
        }

        if (attacker->GetGUID() != t_swing.attacker || victim->GetGUID() != t_swing.victim)
            return;

        t_swing.armed = false;

        if (Player* player = attacker->ToPlayer())
            HandleMeleeHit(player);
    }

    // DoT de mêlée, capacités de mêlée (Sinister Strike, Mutilation, Heroic Strike...)
    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!g_enable || !g_meleeEnable || !attacker || !spellInfo)
            return;

        if (spellInfo->DmgClass != SPELL_DAMAGE_CLASS_MELEE)
            return;

        if (Player* player = attacker->ToPlayer())
            HandleMeleeHit(player);
    }
};

class DotInfinite_Player : public PlayerScript
{
public:
    DotInfinite_Player() : PlayerScript("DotInfinite_Player", { PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> lock(g_meleeMutex);
        g_lastMeleeEval.erase(player->GetGUID());
    }
};

void AddDotInfiniteScripts()
{
    new DotInfinite_World();
    new DotInfinite_Unit();
    new DotInfinite_Player();
}
