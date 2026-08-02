/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// From SC
void AddModSpellAutoTargetScripts();
void AddModSpellAutoTargetCommandScripts();

// Add all
// cf. la convention de nommage AzerothCore : le nom du dossier du module
// (mod-spell-autotarget) avec les tirets remplacés par des underscores.
void Addmod_spell_autotargetScripts()
{
    AddModSpellAutoTargetScripts();
    AddModSpellAutoTargetCommandScripts();
}
