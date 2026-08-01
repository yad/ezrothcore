DELETE FROM `item_template` WHERE `entry` = 70000;
INSERT INTO `item_template` (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,`Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,`InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,`RequiredSkill`,`RequiredSkillRank`,`requiredspell`,`requiredhonorrank`,`RequiredCityRank`,`RequiredReputationFaction`,`RequiredReputationRank`,`maxcount`,`stackable`,`ContainerSlots`,`stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,`stat_type3`,`stat_value3`,`stat_type4`,`stat_value4`,`stat_type5`,`stat_value5`,`stat_type6`,`stat_value6`,`stat_type7`,`stat_value7`,`stat_type8`,`stat_value8`,`stat_type9`,`stat_value9`,`stat_type10`,`stat_value10`,`ScalingStatDistribution`,`ScalingStatValue`,`dmg_min1`,`dmg_max1`,`dmg_type1`,`dmg_min2`,`dmg_max2`,`dmg_type2`,`armor`,`holy_res`,`fire_res`,`nature_res`,`frost_res`,`shadow_res`,`arcane_res`,`delay`,`ammo_type`,`RangedModRange`,`spellid_1`,`spelltrigger_1`,`spellcharges_1`,`spellppmRate_1`,`spellcooldown_1`,`spellcategory_1`,`spellcategorycooldown_1`,`spellid_2`,`spelltrigger_2`,`spellcharges_2`,`spellppmRate_2`,`spellcooldown_2`,`spellcategory_2`,`spellcategorycooldown_2`,`spellid_3`,`spelltrigger_3`,`spellcharges_3`,`spellppmRate_3`,`spellcooldown_3`,`spellcategory_3`,`spellcategorycooldown_3`,`spellid_4`,`spelltrigger_4`,`spellcharges_4`,`spellppmRate_4`,`spellcooldown_4`,`spellcategory_4`,`spellcategorycooldown_4`,`spellid_5`,`spelltrigger_5`,`spellcharges_5`,`spellppmRate_5`,`spellcooldown_5`,`spellcategory_5`,`spellcategorycooldown_5`,`bonding`,`description`,`PageText`,`LanguageID`,`PageMaterial`,`startquest`,`lockid`,`Material`,`sheath`,`RandomProperty`,`RandomSuffix`,`block`,`itemset`,`MaxDurability`,`area`,`Map`,`BagFamily`,`TotemCategory`,`socketColor_1`,`socketContent_1`,`socketColor_2`,`socketContent_2`,`socketColor_3`,`socketContent_3`,`socketBonus`,`GemProperties`,`RequiredDisenchantSkill`,`ArmorDamageModifier`,`duration`,`ItemLimitCategory`,`HolidayId`,`ScriptName`,`DisenchantID`,`FoodType`,`minMoneyLoot`,`maxMoneyLoot`,`flagsCustom`,`VerifiedBuild`) VALUES (70000,15,0,-1,'Flightmaster Whistle',58859,4,64,0,1,0,0,0,-1,-1,1,0,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,110001,0,0,0,-1,0,-1,0,0,0,0,-1,0,-1,0,0,0,0,-1,0,-1,0,0,0,0,-1,0,-1,0,0,0,0,-1,0,-1,1,'Teleports you to the nearest flight master in your area.',0,0,0,0,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0,0,0,0,'flightmaster_whistle',0,0,0,0,0,-1);

DELETE FROM `item_dbc` WHERE `ID` = 70000;
INSERT INTO `item_dbc` (`ID`,`ClassID`,`SubclassID`,`Sound_Override_Subclassid`,`Material`,`DisplayInfoID`,`InventoryType`,`SheatheType`) VALUES (70000,15,0,-1,-1,58859,0,0);

DELETE FROM `spell_script_names` WHERE `spell_id` = 110001;

INSERT INTO `spell_script_names`
(`spell_id`, `ScriptName`)
VALUES
(110001, 'spell_flightmaster_whistle_cast');


DELETE FROM `spell_dbc` WHERE `ID` = 110001;

INSERT INTO `spell_dbc`
(
    `ID`,
    `CastingTimeIndex`,
    `RecoveryTime`,
    `DurationIndex`,
    `RangeIndex`,
    `Effect_1`,
    `ImplicitTargetA_1`,
    `Targets`,
    `Name_Lang_enUS`,
    `Name_Lang_frFR`,
    `Description_Lang_enUS`,
    `Description_Lang_frFR`,
    `EquippedItemClass`
)
VALUES
(
    110001,
    7,
    900000,
    0,
    1,
    77,
    1,
    1,
    'Flightmaster Whistle',
    'Sifflet de maître de vol',
    'Teleports you to the nearest flightmaster.',
    'Vous téléporte auprès du maître de vol le plus proche.',
    -1
);

UPDATE `item_template`
SET
    `displayid` = 15798,
    `Quality` = 3,
    `description` = 'Teleports you to the nearest flightmaster.',
    `ScriptName` = 'flightmaster_whistle'
WHERE `entry` = 70000;

DELETE FROM `item_template_locale`
WHERE `ID` = 70000
AND `locale` = 'frFR';

INSERT INTO `item_template_locale`
(
    `ID`,
    `locale`,
    `Name`,
    `Description`,
    `VerifiedBuild`
)
VALUES
(
    70000,
    'frFR',
    'Sifflet de maître de vol',
    'Vous demandez un transfert jusqu’au maître de vol le plus proche.',
    NULL
);