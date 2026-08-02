/*
 * mod-spell-autotarget - utilitaires partagés
 */

#ifndef MOD_SPELL_AUTOTARGET_SHARED_H
#define MOD_SPELL_AUTOTARGET_SHARED_H

#include "SharedDefines.h"
#include "SpellInfo.h"

namespace ModSpellAutoTarget
{
    // Un sort a-t-il au moins un effet dont la destination est
    // TARGET_DEST_DEST (= position cliquée au sol par le client) ?
    inline bool HasGroundClickDestination(SpellInfo const* spellInfo)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;

            if (spellInfo->Effects[i].TargetA.GetTarget() == TARGET_DEST_DEST ||
                spellInfo->Effects[i].TargetB.GetTarget() == TARGET_DEST_DEST)
                return true;
        }

        return false;
    }
}

#endif // MOD_SPELL_AUTOTARGET_SHARED_H
