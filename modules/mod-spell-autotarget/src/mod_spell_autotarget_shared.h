/*
 * mod-spell-autotarget - utilitaires partagés
 */

#ifndef MOD_SPELL_AUTOTARGET_SHARED_H
#define MOD_SPELL_AUTOTARGET_SHARED_H

#include "SharedDefines.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "Tokenize.h"

#include <string_view>
#include <unordered_set>

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

    inline std::string_view Trim(std::string_view sv)
    {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
            sv.remove_prefix(1);
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
            sv.remove_suffix(1);
        return sv;
    }

    // Parse une liste d'IDs de sorts séparés par des virgules, en tolérant
    // les espaces autour de chaque valeur (ex: "10, 6141, 8427").
    inline std::unordered_set<uint32> ParseSpellIdList(std::string_view list)
    {
        std::unordered_set<uint32> result;
        for (std::string_view token : Acore::Tokenize(list, ',', false))
        {
            if (Optional<uint32> id = Acore::StringTo<uint32>(Trim(token)))
                result.insert(*id);
        }
        return result;
    }
}

#endif // MOD_SPELL_AUTOTARGET_SHARED_H
