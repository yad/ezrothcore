#include "Player.h"
#include "Config.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"

#include <algorithm>

static bool sEnable = true;

static float sAttackSpeedBonus = 30.0f;
static float sDamageReductionPct = 20.0f;

static float sPetAttackSpeedBonus = 30.0f;
static float sPetDamageReductionPct = 20.0f;

// -----------------------------------------------------------------------------
// Joueur
// -----------------------------------------------------------------------------

static void ApplyAttackSpeedBonus(Player* player)
{
    if (!sEnable || sAttackSpeedBonus <= 0.0f)
        return;

    player->ApplyAttackTimePercentMod(BASE_ATTACK, sAttackSpeedBonus, true);
    player->ApplyAttackTimePercentMod(OFF_ATTACK, sAttackSpeedBonus, true);
    player->ApplyAttackTimePercentMod(RANGED_ATTACK, sAttackSpeedBonus, true);
}

// -----------------------------------------------------------------------------
// Pet
// -----------------------------------------------------------------------------

static void ApplyPetAttackSpeedBonus(Guardian* guardian)
{
    if (!guardian || !sEnable || sPetAttackSpeedBonus <= 0.0f)
        return;

    guardian->ApplyAttackTimePercentMod(BASE_ATTACK, sPetAttackSpeedBonus, true);
    guardian->ApplyAttackTimePercentMod(OFF_ATTACK, sPetAttackSpeedBonus, true);
}

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

class PlayerBoostConfig : public WorldScript
{
public:
    PlayerBoostConfig() : WorldScript("PlayerBoostConfig") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        sEnable = sConfigMgr->GetOption<bool>("PlayerBoost.Enable", true);

        sAttackSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.AttackSpeedBonus", 30.0f);

        sDamageReductionPct =
            sConfigMgr->GetOption<float>("PlayerBoost.DamageReductionPct", 20.0f);

        sPetAttackSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.PetAttackSpeedBonus", 30.0f);

        sPetDamageReductionPct =
            sConfigMgr->GetOption<float>("PlayerBoost.PetDamageReductionPct", 20.0f);
    }
};

// -----------------------------------------------------------------------------
// PlayerScript
// -----------------------------------------------------------------------------

class PlayerBoostPlayer : public PlayerScript
{
public:
    PlayerBoostPlayer() : PlayerScript("PlayerBoostPlayer") { }

    void OnPlayerLogin(Player* player) override
    {
        ApplyAttackSpeedBonus(player);
    }

    void OnPlayerCreate(Player* player) override
    {
        ApplyAttackSpeedBonus(player);
    }

    void OnPlayerAfterGuardianInitStatsForLevel(Player* /*player*/, Guardian* guardian) override
    {
        ApplyPetAttackSpeedBonus(guardian);
    }
};

// -----------------------------------------------------------------------------
// UnitScript
// -----------------------------------------------------------------------------

class PlayerBoostUnit : public UnitScript
{
public:
    PlayerBoostUnit() : UnitScript("PlayerBoostUnit") { }

    void ModifyMeleeDamage(Unit* target, Unit* /*attacker*/, uint32& damage) override
    {
        if (!sEnable)
            return;

        if (target->IsPlayer() && sDamageReductionPct > 0.0f)
        {
            float reduction = std::min(sDamageReductionPct, 100.0f);
            damage = uint32(float(damage) * (1.0f - reduction / 100.0f));
        }
        else if (target->IsGuardian() &&
                 target->GetOwnerGUID().IsPlayer() &&
                 sPetDamageReductionPct > 0.0f)
        {
            float reduction = std::min(sPetDamageReductionPct, 100.0f);
            damage = uint32(float(damage) * (1.0f - reduction / 100.0f));
        }
    }

    void ModifySpellDamageTaken(Unit* target, Unit* /*attacker*/, int32& damage,
                                SpellInfo const* /*spellInfo*/) override
    {
        if (!sEnable || damage <= 0)
            return;

        if (target->IsPlayer() && sDamageReductionPct > 0.0f)
        {
            float reduction = std::min(sDamageReductionPct, 100.0f);
            damage = int32(float(damage) * (1.0f - reduction / 100.0f));
        }
        else if (target->IsGuardian() &&
                 target->GetOwnerGUID().IsPlayer() &&
                 sPetDamageReductionPct > 0.0f)
        {
            float reduction = std::min(sPetDamageReductionPct, 100.0f);
            damage = int32(float(damage) * (1.0f - reduction / 100.0f));
        }
    }
};

// -----------------------------------------------------------------------------
// Enregistrement
// -----------------------------------------------------------------------------

void AddPlayerBoostScripts()
{
    new PlayerBoostConfig();
    new PlayerBoostPlayer();
    new PlayerBoostUnit();
}