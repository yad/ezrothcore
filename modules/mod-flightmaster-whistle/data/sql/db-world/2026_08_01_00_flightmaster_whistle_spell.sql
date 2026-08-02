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
    8690,   -- sort Pierre de Foyer réutilisé, filtré côté script sur l'entry de l'objet
    0,
    0,
    -1,
    1,
    'Teleports you to the nearest flightmaster.'
);



-- SPELL SCRIPT
DELETE FROM `spell_script_names`
WHERE `spell_id` = 8690
AND `ScriptName` = 'spell_flightmaster_whistle_cast';

INSERT INTO `spell_script_names`
(
    `spell_id`,
    `ScriptName`
)
VALUES
(
    8690,
    'spell_flightmaster_whistle_cast'
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