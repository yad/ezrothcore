-- ITEM TEMPLATE
DELETE FROM `item_template` WHERE `entry` = 70000;

INSERT INTO `item_template`
(
    `entry`,
    `class`,
    `subclass`,
    `SoundOverrideSubclass`,
    `name`,
    `displayid`,
    `Quality`,
    `Flags`,
    `BuyCount`,
    `BuyPrice`,
    `SellPrice`,
    `InventoryType`,
    `AllowableClass`,
    `AllowableRace`,
    `ItemLevel`,
    `RequiredLevel`,
    `maxcount`,
    `stackable`,
    `spellid_1`,
    `spelltrigger_1`,
    `spellcharges_1`,
    `spellcooldown_1`,
    `bonding`,
    `description`
)
VALUES
(
    70000,
    15,
    0,
    -1,
    'Flightmaster Whistle',
    15798,
    3,
    64,
    1,
    0,
    0,
    0,
    -1,
    -1,
    1,
    0,
    1,
    1,
    110001,
    0,
    0,
    -1,
    1,
    'Teleports you to the nearest flightmaster.'
);



-- ITEM DBC
DELETE FROM `item_dbc`
WHERE `ID` = 70000;

INSERT INTO `item_dbc`
(
    `ID`,
    `ClassID`,
    `SubclassID`,
    `Sound_Override_Subclassid`,
    `Material`,
    `DisplayInfoID`,
    `InventoryType`,
    `SheatheType`
)
VALUES
(
    70000,
    15,
    0,
    -1,
    -1,
    15798,
    0,
    0
);



-- SPELL SCRIPT
DELETE FROM `spell_script_names`
WHERE `spell_id` = 110001;

INSERT INTO `spell_script_names`
(
    `spell_id`,
    `ScriptName`
)
VALUES
(
    110001,
    'spell_flightmaster_whistle_cast'
);



-- SPELL DBC
DELETE FROM `spell_dbc`
WHERE `ID` = 110001;


INSERT INTO `spell_dbc`
(
    `ID`,
    `Targets`,
    `CastingTimeIndex`,
    `RecoveryTime`,
    `DurationIndex`,
    `RangeIndex`,
    `EquippedItemClass`,
    `Effect_1`,
    `ImplicitTargetA_1`,
    `ImplicitTargetB_1`,
    `EffectMiscValue_1`,
    `Name_Lang_enUS`,
    `Name_Lang_frFR`,
    `Description_Lang_enUS`,
    `Description_Lang_frFR`
)
VALUES
(
    110001,
    1,
    1,
    900000,
    0,
    1,
    -1,
    77,
    1,
    0,
    0,
    'Flightmaster Whistle',
    'Sifflet de maître de vol',
    'Teleports you to the nearest flightmaster.',
    'Vous téléporte auprès du maître de vol le plus proche.'
);



-- LOCALE ITEM
DELETE FROM `item_template_locale`
WHERE `ID` = 70000
AND `locale` = 'frFR';


INSERT INTO `item_template_locale`
(
    `ID`,
    `locale`,
    `Name`,
    `Description`
)
VALUES
(
    70000,
    'frFR',
    'Sifflet de maître de vol',
    'Vous demandez un transfert jusqu\'au maître de vol le plus proche.'
);