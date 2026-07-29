#include "Player.h"
#include "Config.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"

#include <algorithm>

static bool sEnable = false;

static float sAttackSpeedBonus = 0.0f;
static float sDamageReductionPct = 0.0f;

static float sPetAttackSpeedBonus = 0.0f;
static float sPetDamageReductionPct = 0.0f;

static float sCastSpeedBonus = 0.0f;
static float sPetCastSpeedBonus = 0.0f;


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

static void ApplyCastSpeedBonus(Player* player)
{
    if (!sEnable || sCastSpeedBonus <= 0.0f)
        return;

    player->ApplyCastTimePercentMod(sCastSpeedBonus, true);
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

static void ApplyPetCastSpeedBonus(Guardian* guardian)
{
    if (!sEnable || sCastSpeedBonus <= 0.0f)
        return;

    guardian->ApplyCastTimePercentMod(sCastSpeedBonus, true);
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
        sEnable = sConfigMgr->GetOption<bool>("PlayerBoost.Enable", false);

        sAttackSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.AttackSpeedBonus", 0.0f);

        sDamageReductionPct =
            sConfigMgr->GetOption<float>("PlayerBoost.DamageReductionPct", 0.0f);

        sPetAttackSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.PetAttackSpeedBonus", 0.0f);

        sPetDamageReductionPct =
            sConfigMgr->GetOption<float>("PlayerBoost.PetDamageReductionPct", 0.0f);

        sCastSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.CastSpeedBonus", 0.0f);

        sPetCastSpeedBonus =
            sConfigMgr->GetOption<float>("PlayerBoost.sPetCastSpeedBonus", 0.0f);
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
        ApplyCastSpeedBonus(player);
    }

    void OnPlayerCreate(Player* player) override
    {
        ApplyAttackSpeedBonus(player);
        ApplyCastSpeedBonus(player);
    }

    void OnPlayerAfterGuardianInitStatsForLevel(Player* /*player*/, Guardian* guardian) override
    {
        ApplyPetAttackSpeedBonus(guardian);
        ApplyPetCastSpeedBonus(guardian);
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

// GlobalScript - SPELL_ATTRS_SPELL_HASTE_AFFECTS_PERIODIC
//
// Ce flag est vérifié dans Unit::ModSpellCastTime pour le cas
// SPELL_DAMAGE_CLASS_NONE, ce qui permet à UNIT_MOD_CAST_SPEED
// (positionné par ApplyCastTimePercentMod) d'y prendre effet.
// Les sorts canalisés sont volontairement exclus.

class PlayerBoostSpell : public GlobalScript
{
public:
    PlayerBoostSpell()
        : GlobalScript("PlayerBoostSpell", { GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR })
    {
    }

    void OnLoadSpellCustomAttr(SpellInfo* spell) override
    {
        if (!spell)
            return;

        // Pierre de foyer
        if (spell->Id == 8690)
        {
            spell->AttributesEx5 |= SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC;
            return;
        }

        // Invocation de monture : sort non canalisé,
        // SPELL_DAMAGE_CLASS_NONE, temps de cast > 0
        // et qui applique l'aura SPELL_AURA_MOUNTED.
        if (!spell->IsChanneled() &&
            spell->DmgClass == SPELL_DAMAGE_CLASS_NONE &&
            spell->CastTimeEntry &&
            spell->CastTimeEntry->CastTime > 0 &&
            spell->HasAura(SPELL_AURA_MOUNTED))
        {
            spell->AttributesEx5 |= SPELL_ATTR5_SPELL_HASTE_AFFECTS_PERIODIC;
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
    new PlayerBoostSpell();
}
