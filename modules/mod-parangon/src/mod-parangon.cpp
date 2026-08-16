/*
 * mod-parangon — paliers au-dela du seuil : bonus de stats et equite de combat
 * contre le contenu sous-niveau. XP et level-up geres entierement en C++
 * (PLAYER_XP natif, GiveLevel manuel, pas de table SQL custom).
 */

#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Unit.h"
#include "Creature.h"
#include "Formulas.h"
#include "DBCStores.h"
#include "Group.h"
#include "World.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "CellImpl.h"
#include "CreatureAI.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <mutex>

namespace ParangonConfig
{
    bool   Enabled = true;
    uint32 Threshold = 60;
    uint32 MaxLevel = 250;
    float  StatsPerLevelPct = 1.0f;   // % de bonus de stats par palier
    uint32 XPBaseRequirement = 50000;  // XP requise pour le 1er palier parangon
    float  XPGrowthPct = 5.0f;   // croissance en % de l'XP requise par palier
    float  QuestXPCapPct = 5.0f;   // plafond de l'XP de quete, en % de l'XP requise pour le palier courant
    bool   ForceAggroEnabled = true;   // force l'engagement des monstres hostiles sous le seuil a proximite
    float  ForceAggroRangeYards = 20.0f; // portee fixe utilisee a la place de la portee native (qui depend du niveau reel)
    uint32 ForceAggroCheckIntervalMs = 1000; // frequence du scan de proximite, par joueur
    float  CombatEquityMaxFactor = 5.0f;     // plafond du multiplicateur
    bool   BypassProgressionXPBlock = true;  // contourne un blocage externe de l'XP sous le seuil (ex: mod-individual-progression) pour les kills, en gerant nous-memes PLAYER_XP
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static bool IsParangon(Player* player)
{
    return player && player->GetLevel() >= ParangonConfig::Threshold;
}

static uint32 GetParangonLevel(Player* player)
{
    if (!IsParangon(player))
        return 0;

    return player->GetLevel() - ParangonConfig::Threshold;
}

// Retourne le joueur parangon controlant l'unite (lui-meme ou proprietaire du familier).
static Player* GetControllingParangonPlayer(Unit* unit)
{
    if (!unit)
        return nullptr;

    if (unit->GetTypeId() == TYPEID_PLAYER)
        return IsParangon(unit->ToPlayer()) ? unit->ToPlayer() : nullptr;

    if (Unit* owner = unit->GetOwner())
    {
        if (owner->GetTypeId() == TYPEID_PLAYER)
            return IsParangon(owner->ToPlayer()) ? owner->ToPlayer() : nullptr;
    }

    return nullptr;
}

// Ratio k(joueur)/k(mob) des constantes d'armure WotLK — emule un echange
// de degats comme si la creature etait au niveau du joueur.
static float GetCombatEquityFactor(Player* player, Creature* mob)
{
    if (!player || !mob)
        return 1.0f;

    uint32 mobLevel    = mob->GetLevel();
    uint32 playerLevel = player->GetLevel();

    if (mobLevel >= playerLevel)
        return 1.0f;

    auto armorConstant = [](uint32 level) -> float {
        return level < 60 ? 400.0f + 85.0f * static_cast<float>(level)
                          : 467.5f  * static_cast<float>(level) - 22167.5f;
    };

    float kPlayer = armorConstant(playerLevel);
    float kMob    = armorConstant(mobLevel);

    if (kMob <= 0.0f)
        return ParangonConfig::CombatEquityMaxFactor;

    return std::min(kPlayer / kMob, ParangonConfig::CombatEquityMaxFactor);
}

// XPBaseRequirement * (1 + XPGrowthPct%)^parangonLevel
static uint32 GetParangonXPRequired(uint32 parangonLevel)
{
    float growth = 1.0f + (ParangonConfig::XPGrowthPct / 100.0f);
    double required = static_cast<double>(ParangonConfig::XPBaseRequirement) * std::pow(growth, static_cast<double>(parangonLevel));
    return static_cast<uint32>(required);
}

// Applique le bonus elite/boss, Rate.XP.Kill et facteur de groupe — reproduit Acore::XP::Gain() au-dessus de BaseGain().
static uint32 ApplyKillXPMultipliers(Player* player, Unit* victim, uint32 gain)
{
    if (gain == 0)
        return 0;

    if (Creature* creature = victim->ToCreature())
    {
        bool isEliteOrBoss = creature->isElite() || creature->isWorldBoss();
        if (isEliteOrBoss)
        {
            float eliteBonus = creature->GetMap() && creature->GetMap()->IsDungeon() ? 2.75f : 2.0f;
            gain = static_cast<uint32>(gain * eliteBonus);
        }
    }

    gain = static_cast<uint32>(gain * sWorld->getRate(RATE_XP_KILL));

    if (Group* group = player->GetGroup())
    {
        uint32 memberCount = group->GetMembersCount();
        gain = static_cast<uint32>(gain * Acore::XP::xp_in_group_rate(memberCount, group->isRaidGroup()));
    }

    return gain;
}

// BaseGain avec le niveau de la victime force au niveau du joueur (les kills "gris" donneraient 0 autrement).
static uint32 ComputeParangonXPGain(Player* player, Unit* victim)
{
    if (!player || !victim)
        return 0;

    if (victim->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = victim->ToCreature();
        if (creature && (creature->IsTotem() || creature->IsPet() || creature->IsCritter()))
            return 0;
    }

    uint8 level = player->GetLevel();
    ContentLevels content = GetContentLevelsForMapAndZone(player->GetMapId(), player->GetZoneId());
    uint32 gain = Acore::XP::BaseGain(level, level, content);
    return ApplyKillXPMultipliers(player, victim, gain);
}

// Gain d'XP natif (niveaux reels), utilise pour le pont de progression sous le seuil.
static uint32 ComputeNativeKillXPGain(Player* player, Unit* victim)
{
    if (!player || !victim)
        return 0;

    if (victim->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = victim->ToCreature();
        if (creature && (creature->IsTotem() || creature->IsPet() || creature->IsCritter()))
            return 0;
    }

    ContentLevels content = GetContentLevelsForMapAndZone(player->GetMapId(), player->GetZoneId());
    uint32 gain = Acore::XP::BaseGain(player->GetLevel(), victim->GetLevel(), content);
    return ApplyKillXPMultipliers(player, victim, gain);
}

// ---------------------------------------------------------------------
// WorldScript : chargement de la config
// ---------------------------------------------------------------------

class Parangon_WorldScript : public WorldScript
{
public:
    Parangon_WorldScript() : WorldScript("Parangon_WorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
        }) {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        ParangonConfig::Enabled = sConfigMgr->GetOption<bool>("Parangon.Enable", true);
        ParangonConfig::Threshold = sConfigMgr->GetOption<uint32>("Parangon.Threshold", 60);
        ParangonConfig::MaxLevel = sConfigMgr->GetOption<uint32>("Parangon.MaxLevel", 250);
        ParangonConfig::StatsPerLevelPct = sConfigMgr->GetOption<float>("Parangon.StatsPerLevelPct", 1.0f);
        ParangonConfig::XPBaseRequirement = sConfigMgr->GetOption<uint32>("Parangon.XPBaseRequirement", 50000);
        ParangonConfig::XPGrowthPct = sConfigMgr->GetOption<float>("Parangon.XPGrowthPct", 5.0f);
        ParangonConfig::QuestXPCapPct = sConfigMgr->GetOption<float>("Parangon.QuestXPCapPct", 5.0f);
        ParangonConfig::ForceAggroEnabled = sConfigMgr->GetOption<bool>("Parangon.ForceAggroEnabled", true);
        ParangonConfig::ForceAggroRangeYards = sConfigMgr->GetOption<float>("Parangon.ForceAggroRangeYards", 20.0f);
        ParangonConfig::ForceAggroCheckIntervalMs = sConfigMgr->GetOption<uint32>("Parangon.ForceAggroCheckIntervalMs", 1000);
        ParangonConfig::CombatEquityMaxFactor = sConfigMgr->GetOption<float>("Parangon.CombatEquityMaxFactor", 5.0f);
        ParangonConfig::BypassProgressionXPBlock = sConfigMgr->GetOption<bool>("Parangon.BypassProgressionXPBlock", true);
    }
};

// ---------------------------------------------------------------------
// PlayerScript : bonus de stats + XP/leveling geres en C++ au-dela du seuil
// ---------------------------------------------------------------------

class Parangon_PlayerScript : public PlayerScript
{
public:
    Parangon_PlayerScript() : PlayerScript("Parangon_PlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
        PLAYERHOOK_ON_LOGOUT
        }) {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!ParangonConfig::Enabled)
            return;

        // UnitMods non persistes : applique directement le bonus du palier courant.
        uint32 parangonLevel = GetParangonLevel(player);
        if (parangonLevel > 0)
            ApplyParangonStatModifier(player, parangonLevel, true);

        if (IsParangon(player))
        {
            // Corrige PLAYER_NEXT_LEVEL_XP que le coeur recalcule depuis sa propre table.
            uint32 required = GetParangonXPRequired(parangonLevel);
            player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, required);
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!ParangonConfig::Enabled)
            return;

        uint32 oldParangonLevel = (oldLevel >= ParangonConfig::Threshold) ? (oldLevel - ParangonConfig::Threshold) : 0;
        uint32 newParangonLevel = GetParangonLevel(player);

        if (oldParangonLevel == newParangonLevel)
            return;

        if (oldParangonLevel > 0)
            ApplyParangonStatModifier(player, oldParangonLevel, false);

        if (newParangonLevel > 0)
            ApplyParangonStatModifier(player, newParangonLevel, true);
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!ParangonConfig::Enabled || !IsParangon(player))
            return; // sous le seuil : laisse le coeur gerer nativement

        if (xpSource == XPSOURCE_KILL)
        {
            // Les kills sont traites dans OnPlayerCreatureKill (couvre aussi les kills "gris").
            amount = 0;
            return;
        }

        uint32 gain = 0;
        // Quetes et exploration : plafond a QuestXPCapPct % de l'XP du palier courant.
        if (xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF || xpSource == XPSOURCE_EXPLORE)
        {
            uint32 required = GetParangonXPRequired(GetParangonLevel(player));
            uint32 maxGain = static_cast<uint32>(required * (ParangonConfig::QuestXPCapPct / 100.0f));
            gain = std::min(amount, maxGain);
        }
        // XPSOURCE_BATTLEGROUND : laisse a 0 (PvP non couvert par design).

        // player_xp_for_level n'est pas etendue au-dela du seuil : bloque le level-up natif.
        amount = 0;

        if (gain > 0)
            GrantParangonXP(player, gain);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        HandleCreatureKillXP(killer, killed, "OnPlayerCreatureKill");
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* killed) override
    {
        HandleCreatureKillXP(petOwner, killed, "OnPlayerCreatureKilledByPet");
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(m_aggroCheckTimersMutex);
        m_aggroCheckTimers.erase(player->GetGUID());
    }

private:
    void HandleCreatureKillXP(Player* killer, Creature* killed, char const* source)
    {
        if (!ParangonConfig::Enabled || !killer || !killed)
            return;

        if (IsParangon(killer))
        {
            uint32 gain = ComputeParangonXPGain(killer, killed);
            if (gain > 0)
                GrantParangonXP(killer, gain);
            return;
        }

        // BypassProgressionXPBlock : contourne un module bloquant GiveXP() (ex: mod-individual-progression).
        if (ParangonConfig::BypassProgressionXPBlock)
            GrantBridgeXP(killer, killed);
    }

    // Reduit l'or de loot de creature avec le meme facteur d'equite que les degats.
    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (!ParangonConfig::Enabled || !IsParangon(player) || !loot)
            return;

        Creature* creature = ObjectAccessor::GetCreature(*player, player->GetLootGUID());
        if (!creature)
            return;

        float factor = GetCombatEquityFactor(player, creature);
        if (factor <= 1.0f)
            return; // creature au niveau du joueur ou au-dessus : pas de correction

        // TODO: confirmer que le champ s'appelle bien "gold" sur cette version du core.
        loot->gold = static_cast<uint32>(loot->gold / factor);
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!ParangonConfig::Enabled || !ParangonConfig::ForceAggroEnabled || !IsParangon(player))
            return;

        ObjectGuid guid = player->GetGUID();
        bool shouldCheck = false;

        {
            std::lock_guard<std::mutex> lock(m_aggroCheckTimersMutex);
            uint32& timer = m_aggroCheckTimers[guid];

            if (timer <= p_time)
            {
                timer = ParangonConfig::ForceAggroCheckIntervalMs;
                shouldCheck = true;
            }
            else
            {
                timer -= p_time;
            }
        }

        if (shouldCheck)
            ForceAggroNearbyMobs(player);
    }

private:
    // Singleton partage entre tous les joueurs : acces concurrent protege par mutex.
    std::mutex m_aggroCheckTimersMutex;
    std::unordered_map<ObjectGuid, uint32> m_aggroCheckTimers;

    // Force l'aggro dans un rayon fixe ; ne touche que les creatures deja hostiles.
    void ForceAggroNearbyMobs(Player* player)
    {
        std::list<Creature*> nearbyCreatures;

        Acore::AnyUnitInObjectRangeCheck check(player, ParangonConfig::ForceAggroRangeYards);
        Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(player, nearbyCreatures, check);
        Cell::VisitObjects(player, searcher, ParangonConfig::ForceAggroRangeYards);

        for (Creature* creature : nearbyCreatures)
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat())
                continue;

            if (creature->GetLevel() >= ParangonConfig::Threshold)
                continue; // deja au niveau seuil ou au-dessus, l'aggro native suffit

            if (!creature->IsHostileTo(player))
                continue; // ne rend pas hostile un monstre passif par template

            if (CreatureAI* ai = creature->AI())
                ai->AttackStart(player);
        }
    }

private:
    // ApplyStatPctModifier : pct positif pour ajouter, negatif pour retirer (pas de bool apply).
    void ApplyParangonStatModifier(Player* player, uint32 parangonLevel, bool apply)
    {
        float pct = parangonLevel * ParangonConfig::StatsPerLevelPct;
        if (pct <= 0.0f)
            return;

        if (!apply)
            pct = -pct;

        static UnitMods const statMods[MAX_STATS] =
        {
            UNIT_MOD_STAT_STRENGTH,
            UNIT_MOD_STAT_AGILITY,
            UNIT_MOD_STAT_STAMINA,
            UNIT_MOD_STAT_INTELLECT,
            UNIT_MOD_STAT_SPIRIT
        };

        for (uint8 i = 0; i < MAX_STATS; ++i)
            player->ApplyStatPctModifier(statMods[i], TOTAL_PCT, pct);

        player->UpdateAllStats();
    }

    // Gere l'XP sous le seuil en court-circuitant GiveXP() pour eviter un blocage externe.
    void GrantBridgeXP(Player* player, Creature* victim)
    {
        uint32 gain = ComputeNativeKillXPGain(player, victim);
        if (gain == 0)
            return;

        uint32 currentXP = player->GetUInt32Value(PLAYER_XP);
        uint32 newXP = currentXP + gain;

        uint32 required = sObjectMgr->GetXPForLevel(player->GetLevel());

        while (required > 0 && newXP >= required && player->GetLevel() < ParangonConfig::Threshold)
        {
            newXP -= required;
            player->GiveLevel(player->GetLevel() + 1);
            required = sObjectMgr->GetXPForLevel(player->GetLevel());
        }

        SyncNativeXPBar(player, newXP, required);
    }

    // Ecrit directement sans passer par GiveXP() : aucun level-up natif declenche.
    void SyncNativeXPBar(Player* player, uint32 currentXP, uint32 requiredXP)
    {
        player->SetUInt32Value(PLAYER_XP, currentXP);
        player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, requiredXP);
    }

    void GrantParangonXP(Player* player, uint32 gain)
    {
        uint32 currentXP = player->GetUInt32Value(PLAYER_XP);
        uint32 newXP = currentXP + gain;

        uint32 parangonLevel = GetParangonLevel(player);
        uint32 required = GetParangonXPRequired(parangonLevel);

        bool leveledUp = false;

        while (newXP >= required && player->GetLevel() < ParangonConfig::MaxLevel)
        {
            newXP -= required;
            player->GiveLevel(player->GetLevel() + 1); // peut corrompre PLAYER_XP — SyncNativeXPBar corrige juste apres.
            leveledUp = true;

            parangonLevel = GetParangonLevel(player);
            required = GetParangonXPRequired(parangonLevel);
        }

        // A MaxLevel, plafonne PLAYER_XP a required-1 pour eviter l'accumulation infinie.
        if (player->GetLevel() >= ParangonConfig::MaxLevel && newXP >= required)
            newXP = required - 1;

        SyncNativeXPBar(player, newXP, required);
    }
};

// ---------------------------------------------------------------------
// UnitScript : equite de combat contre le contenu sous le seuil
// ---------------------------------------------------------------------

class Parangon_UnitScript : public UnitScript
{
public:
    Parangon_UnitScript() : UnitScript("Parangon_UnitScript") {}

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        ApplyEquityFactorUnsigned(target, attacker, damage);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        ApplyEquityFactorUnsigned(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        uint32 unsignedDamage = static_cast<uint32>(damage);
        ApplyEquityFactorUnsigned(target, attacker, unsignedDamage);
        damage = static_cast<int32>(unsignedDamage);
    }

private:
    void ApplyEquityFactorUnsigned(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!ParangonConfig::Enabled || !target || !attacker || damage == 0)
            return;

        if (Player* player = GetControllingParangonPlayer(attacker))
        {
            float factor = GetCombatEquityFactor(player, target->ToCreature());
            if (factor > 1.0f)
                damage = static_cast<uint32>(damage / factor);
        }
        else if (Player* player = GetControllingParangonPlayer(target))
        {
            float factor = GetCombatEquityFactor(player, attacker->ToCreature());
            if (factor > 1.0f)
                damage = static_cast<uint32>(damage * factor);
        }
    }
};

// ---------------------------------------------------------------------
// Enregistrement des scripts
// ---------------------------------------------------------------------

void Addmod_parangonScripts()
{
    new Parangon_WorldScript();
    new Parangon_PlayerScript();
    new Parangon_UnitScript();
}
