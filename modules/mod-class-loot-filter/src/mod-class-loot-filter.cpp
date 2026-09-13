/*
 * mod-class-loot-filter
 * ----------------------
 * Deux comportements DISTINCTS :
 *
 * 1) BoE (Bind on Equip) sur créatures/coffres, tous types de mobs :
 *    au moment du loot (PlayerScript::OnLootItem), si l'objet n'est pas
 *    utilisable/préféré pour la classe du joueur, il est retiré, puis :
 *      a) tente de le remplacer par un autre objet du MÊME loot encore
 *         disponible qui EST utilisable ;
 *      b) sinon, rend l'objet d'origine.
 *
 * 2) "Smart loot" BoP (Bind on Pickup) sur les BOSS de donjon/raid
 *    uniquement : au moment de la génération de la table de butin
 *    (GlobalScript::OnItemRoll), la chance de drop de chaque pièce
 *    d'armure/arme soulbound est augmentée si elle est utilisable par au
 *    moins un joueur HUMAIN du groupe (bots exclus), et réduite sinon.
 *    Cela influence uniquement si l'objet apparaît dans le butin, PAS qui
 *    gagne le tirage besoin/cupidité ensuite (toujours géré par le core).
 *
 * ATTENTION - à vérifier avant compilation sur votre arbre exact :
 *   - Signature de PlayerScript::OnLootItem (Player*, Item*, uint32, ObjectGuid)
 *   - Signature de GlobalScript::OnItemRoll (Player const*, LootStoreItem const*,
 *     float& chance, Loot&, LootStore const&) -> bool
 *   - Enum ItemBondingType (ItemTemplate.h) : NO_BIND, BIND_WHEN_PICKED_UP,
 *     BIND_WHEN_EQUIPPED, BIND_WHEN_USE, BIND_QUEST_ITEM. Attention, selon
 *     la version du core, la valeur BoE peut s'appeler BIND_WHEN_EQUIPPED
 *     (deux P) ou BIND_WHEN_EQUIPED (un seul P) — grep pour confirmer.
 *   - Membre Loot::sourceWorldObjectGUID (LootMgr.h)
 *   - Existence de Player::StoreNewItemInBestSlots(uint32 itemId, uint32 count)
 *   - Membres de LootItem (itemid, is_looted, AllowedForPlayer)
 *   - ObjectGuid::IsCreatureOrVehicle() / IsGameObject()
 *   - Player::HasSkill(uint32 skill) et les constantes SKILL_MAIL /
 *     SKILL_PLATE_MAIL (SharedDefines.h), utilisées pour la préférence
 *     stricte d'armure (repli mailles si pas encore skill plaques, etc.)
 *   - Group::GetFirstMember() / GroupReference::next() / GetSource()
 *     (Group.h), pour lister les membres du groupe
 *   - WorldSession::IsBot(), sous #ifdef MOD_PLAYERBOTS (mod-playerbots) -
 *     compile à false si le module playerbots n'est pas présent, aucune
 *     dépendance dure sur un header playerbots externe.
 * Grep ces symboles dans votre core local si une erreur de build apparaît,
 * comme d'habitude, et on corrige au besoin.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "LootMgr.h"
#include "Config.h"
#include "Chat.h"
#include "SharedDefines.h"
#include "Random.h"

#include <initializer_list>
#include <vector>
#include <algorithm>

namespace
{
    // ---------------------------------------------------------------------
    // Ancienne table "permissive" (tout ce que la classe PEUT porter).
    // Conservée pour ClassLootFilter.StrictArmorPreference = 0.
    // ---------------------------------------------------------------------
    bool CanWearArmorSubclass(uint8 playerClass, uint32 subclass)
    {
        switch (subclass)
        {
            case ITEM_SUBCLASS_ARMOR_CLOTH:
                return true; // tissu -> toutes les classes

            case ITEM_SUBCLASS_ARMOR_LEATHER:
                switch (playerClass)
                {
                    case CLASS_ROGUE:
                    case CLASS_DRUID:
                    case CLASS_HUNTER:
                    case CLASS_SHAMAN:
                    case CLASS_WARRIOR:
                    case CLASS_PALADIN:
                    case CLASS_DEATH_KNIGHT:
                        return true;
                    default:
                        return false;
                }

            case ITEM_SUBCLASS_ARMOR_MAIL:
                switch (playerClass)
                {
                    case CLASS_HUNTER:
                    case CLASS_SHAMAN:
                    case CLASS_WARRIOR:
                    case CLASS_PALADIN:
                    case CLASS_DEATH_KNIGHT:
                        return true;
                    default:
                        return false;
                }

            case ITEM_SUBCLASS_ARMOR_PLATE:
                switch (playerClass)
                {
                    case CLASS_WARRIOR:
                    case CLASS_PALADIN:
                    case CLASS_DEATH_KNIGHT:
                        return true;
                    default:
                        return false;
                }

            default:
                return true;
        }
    }

    // ---------------------------------------------------------------------
    // Mode strict : une seule sous-classe "préférée" par classe, avec repli
    // automatique si le skill correspondant n'est pas encore acquis (ex :
    // guerrier/paladin < niveau plaques -> mailles).
    // ---------------------------------------------------------------------
    uint32 GetPreferredArmorSubclass(Player* player)
    {
        switch (player->getClass())
        {
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                return ITEM_SUBCLASS_ARMOR_CLOTH;

            case CLASS_ROGUE:
            case CLASS_DRUID:
                return ITEM_SUBCLASS_ARMOR_LEATHER;

            case CLASS_HUNTER:
            case CLASS_SHAMAN:
                if (player->HasSkill(SKILL_MAIL))
                    return ITEM_SUBCLASS_ARMOR_MAIL;
                return ITEM_SUBCLASS_ARMOR_LEATHER;

            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                if (player->HasSkill(SKILL_PLATE_MAIL))
                    return ITEM_SUBCLASS_ARMOR_PLATE;
                return ITEM_SUBCLASS_ARMOR_MAIL;

            default:
                return ITEM_SUBCLASS_ARMOR_CLOTH;
        }
    }

    bool IsArmorSubclassAccepted(Player* player, uint32 subclass)
    {
        uint8 playerClass = player->getClass();

        switch (subclass)
        {
            case ITEM_SUBCLASS_ARMOR_MISC: // capes, etc. -> jamais restreint
                return true;

            case ITEM_SUBCLASS_ARMOR_SHIELD:
                return playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN || playerClass == CLASS_SHAMAN;

            case ITEM_SUBCLASS_ARMOR_LIBRAM:
                return playerClass == CLASS_PALADIN;

            case ITEM_SUBCLASS_ARMOR_IDOL:
                return playerClass == CLASS_DRUID;

            case ITEM_SUBCLASS_ARMOR_TOTEM:
                return playerClass == CLASS_SHAMAN;

            case ITEM_SUBCLASS_ARMOR_SIGIL:
                return playerClass == CLASS_DEATH_KNIGHT;

            case ITEM_SUBCLASS_ARMOR_CLOTH:
            case ITEM_SUBCLASS_ARMOR_LEATHER:
            case ITEM_SUBCLASS_ARMOR_MAIL:
            case ITEM_SUBCLASS_ARMOR_PLATE:
                if (!sConfigMgr->GetOption<bool>("ClassLootFilter.StrictArmorPreference", true))
                    return CanWearArmorSubclass(playerClass, subclass);
                return subclass == GetPreferredArmorSubclass(player);

            default:
                return true; // sous-classe inconnue -> ne pas bloquer par prudence
        }
    }

    // ---------------------------------------------------------------------
    // Table de proficience d'armes par classe (approximatif, WotLK classique).
    // Désactivée par défaut (ClassLootFilter.FilterWeapons = 0) : à activer
    // volontairement une fois la table validée pour votre serveur.
    // ---------------------------------------------------------------------
    bool CanUseWeaponSubclass(uint8 playerClass, uint32 subclass)
    {
        auto inList = [subclass](std::initializer_list<uint32> list)
        {
            for (uint32 v : list)
                if (v == subclass)
                    return true;
            return false;
        };

        switch (playerClass)
        {
            case CLASS_WARRIOR:
                return subclass != ITEM_SUBCLASS_WEAPON_WAND; // tout sauf baguette

            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                return inList({ ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_AXE2,
                                 ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_MACE2,
                                 ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_SWORD2,
                                 ITEM_SUBCLASS_WEAPON_POLEARM });

            case CLASS_HUNTER:
                return inList({ ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_AXE2,
                                 ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN,
                                 ITEM_SUBCLASS_WEAPON_CROSSBOW, ITEM_SUBCLASS_WEAPON_DAGGER,
                                 ITEM_SUBCLASS_WEAPON_FIST, ITEM_SUBCLASS_WEAPON_POLEARM,
                                 ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_SWORD2,
                                 ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_THROWN,
                                 ITEM_SUBCLASS_WEAPON_SPEAR });

            case CLASS_ROGUE:
                return inList({ ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_FIST,
                                 ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_MACE,
                                 ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_THROWN,
                                 ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN,
                                 ITEM_SUBCLASS_WEAPON_CROSSBOW });

            case CLASS_PRIEST:
                return inList({ ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_MACE,
                                 ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_WAND });

            case CLASS_SHAMAN:
                return inList({ ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_AXE2,
                                 ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_MACE2,
                                 ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_FIST,
                                 ITEM_SUBCLASS_WEAPON_STAFF });

            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return inList({ ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_SWORD,
                                 ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_WAND });

            case CLASS_DRUID:
                return inList({ ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_MACE,
                                 ITEM_SUBCLASS_WEAPON_MACE2, ITEM_SUBCLASS_WEAPON_STAFF,
                                 ITEM_SUBCLASS_WEAPON_FIST });

            default:
                return true;
        }
    }

    // Détecte un personnage contrôlé par mod-playerbots (bot d'équipe ou bot
    // aléatoire). Le bloc #ifdef rend le module portable même sans
    // mod-playerbots dans l'arbre (retourne alors toujours false).
    bool IsPlayerBot(Player* player)
    {
#ifdef MOD_PLAYERBOTS
        if (player && player->GetSession())
            return player->GetSession()->IsBot();
#endif
        return false;
    }

    bool IsUsableByClass(ItemTemplate const* proto, Player* player)
    {
        uint8 playerClass = player->getClass();

        // 1) Restriction explicite en base (reliques, objets réservés à une classe...)
        if (proto->AllowableClass != -1)
        {
            uint32 classMask = 1u << (playerClass - 1);
            if (!(proto->AllowableClass & classMask))
                return false;
        }

        if (proto->Class == ITEM_CLASS_ARMOR)
        {
            if (!sConfigMgr->GetOption<bool>("ClassLootFilter.FilterArmor", true))
                return true;
            return IsArmorSubclassAccepted(player, proto->SubClass);
        }

        if (proto->Class == ITEM_CLASS_WEAPON)
        {
            if (!sConfigMgr->GetOption<bool>("ClassLootFilter.FilterWeapons", false))
                return true;
            return CanUseWeaponSubclass(playerClass, proto->SubClass);
        }

        return true;
    }

    Loot* GetLootFromGuid(Player* player, ObjectGuid const& lootguid)
    {
        if (lootguid.IsCreatureOrVehicle())
        {
            if (Creature* creature = ObjectAccessor::GetCreature(*player, lootguid))
                return &creature->loot;
        }
        else if (lootguid.IsGameObject())
        {
            if (GameObject* go = ObjectAccessor::GetGameObject(*player, lootguid))
                return &go->loot;
        }

        return nullptr;
    }

    // Le butin provient-il d'un boss de donjon/raid ? (drapeau flags_extra
    // "Dungeon Boss" côté template de créature — n'englobe pas les boss de
    // monde ouvert, qui ne sont pas concernés par la demande).
    bool IsDungeonOrRaidBossLoot(Player* player, ObjectGuid const& lootguid)
    {
        if (!lootguid.IsCreatureOrVehicle())
            return false;

        Creature* creature = ObjectAccessor::GetCreature(*player, lootguid);
        if (!creature)
            return false;

        return creature->IsDungeonBoss();
    }

    // Liste les joueurs humains (hors playerbots) pertinents pour ce butin :
    // les membres en ligne du groupe du joueur, ou lui seul s'il est solo.
    std::vector<Player*> GetHumanGroupMembers(Player* player)
    {
        std::vector<Player*> humans;
        bool excludeBots = sConfigMgr->GetOption<bool>("ClassLootFilter.ExcludeBots", true);

        if (Group* group = player->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || !member->IsInWorld())
                    continue;
                if (excludeBots && IsPlayerBot(member))
                    continue;
                humans.push_back(member);
            }
        }
        else if (!(excludeBots && IsPlayerBot(player)))
        {
            humans.push_back(player);
        }

        return humans;
    }

    // Cherche, dans le même butin, un autre objet (armure/arme) de même
    // qualité, encore disponible et utilisable par la classe du joueur.
    bool TryGrantAlternateItem(Player* player, ObjectGuid const& lootguid, uint32 excludeItemId,
        uint32 requiredQuality)
    {
        Loot* loot = GetLootFromGuid(player, lootguid);
        if (!loot)
            return false;

        std::vector<LootItem*> candidates;
        for (LootItem& li : loot->items)
        {
            if (li.is_looted || li.itemid == excludeItemId)
                continue;

            ItemTemplate const* altProto = sObjectMgr->GetItemTemplate(li.itemid);
            if (!altProto)
                continue;

            if (altProto->Class != ITEM_CLASS_ARMOR && altProto->Class != ITEM_CLASS_WEAPON)
                continue;

            if (altProto->Quality != requiredQuality)
                continue;

            if (!li.AllowedForPlayer(player, lootguid))
                continue;

            if (!IsUsableByClass(altProto, player))
                continue;

            candidates.push_back(&li);
        }

        if (candidates.empty())
            return false;

        LootItem* selected = candidates[urand(0, static_cast<uint32>(candidates.size() - 1))];
        if (player->StoreNewItemInBestSlots(selected->itemid, 1))
        {
            selected->is_looted = true;

            if (sConfigMgr->GetOption<bool>("ClassLootFilter.Announce", true))
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000[ClassLootFilter]|r Objet remplacé par une pièce adaptée à votre classe.");

            return true;
        }

        return false;
    }

}

class ClassLootFilter_GlobalScript : public GlobalScript
{
public:
    ClassLootFilter_GlobalScript() : GlobalScript("ClassLootFilter_GlobalScript") { }

    // "Smart loot" : pondère la chance de drop des pièces SOULBOUND (BoP)
    // sur les boss de donjon/raid selon les classes des joueurs humains du
    // groupe. Ne touche jamais au BoE (géré par OnLootItem) ni aux boss de
    // monde ouvert / trashs.
    bool OnItemRoll(Player const* player, LootStoreItem const* lootStoreItem, float& chance, Loot& loot, LootStore const& /*store*/) override
    {
        if (!sConfigMgr->GetOption<bool>("ClassLootFilter.SmartLootEnable", true))
            return true;

        if (!player || !lootStoreItem)
            return true;

        Player* nonConstPlayer = const_cast<Player*>(player);

        Creature* creature = ObjectAccessor::GetCreature(*nonConstPlayer, loot.sourceWorldObjectGUID);
        if (!creature || !creature->IsDungeonBoss())
            return true; // uniquement les boss de donjon/raid

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(lootStoreItem->itemid);
        if (!proto)
            return true;

        // Uniquement armures/armes soulbound (BoP) - le BoE reste géré par OnLootItem
        if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
            return true;

        if (proto->Bonding != BIND_WHEN_PICKED_UP)
            return true;

        std::vector<Player*> humans = GetHumanGroupMembers(nonConstPlayer);
        if (humans.empty())
            return true; // aucun humain identifiable -> ne pas biaiser

        bool usefulForAtLeastOneHuman = false;
        for (Player* human : humans)
        {
            if (IsUsableByClass(proto, human))
            {
                usefulForAtLeastOneHuman = true;
                break;
            }
        }

        float factor = usefulForAtLeastOneHuman
            ? sConfigMgr->GetOption<float>("ClassLootFilter.SmartLootBoostFactor", 2.0f)
            : sConfigMgr->GetOption<float>("ClassLootFilter.SmartLootPenaltyFactor", 0.3f);

        chance = std::max(0.0f, std::min(chance * factor, 100.0f));

        return true;
    }
};

class ClassLootFilter_PlayerScript : public PlayerScript
{
public:
    ClassLootFilter_PlayerScript() : PlayerScript("ClassLootFilter_PlayerScript") { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        if (!sConfigMgr->GetOption<bool>("ClassLootFilter.Enable", true))
            return;

        if (!player || !item)
            return;

        if (sConfigMgr->GetOption<bool>("ClassLootFilter.ExcludeBots", true) && IsPlayerBot(player))
            return; // on ne touche pas au loot des playerbots

        bool isBossLoot = IsDungeonOrRaidBossLoot(player, lootguid);

        if (sConfigMgr->GetOption<bool>("ClassLootFilter.OnlyBosses", false) && !isBossLoot)
            return; // mode restreint aux boss de donjon/raid uniquement

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;

        // On ne s'occupe que des armures et armes
        if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
            return;

        if (sConfigMgr->GetOption<bool>("ClassLootFilter.OnlyBindOnEquip", true)
            && proto->Bonding != BIND_WHEN_EQUIPPED)
            return;

        uint32 minIlvl = sConfigMgr->GetOption<uint32>("ClassLootFilter.MinItemLevel", 1);
        if (proto->ItemLevel < minIlvl)
            return;

        if (IsUsableByClass(proto, player))
            return; // rien a faire, la piece convient a la classe

        // La piece ne convient pas : on la retire de l'inventaire du joueur
        uint8 bag = item->GetBagSlot();
        uint8 slot = item->GetSlot();
        uint32 itemId = proto->ItemId;
        uint32 removedCount = item->GetCount();

        player->DestroyItem(bag, slot, true);

        bool replaced = false;
        if (sConfigMgr->GetOption<bool>("ClassLootFilter.TryAlternateItem", true))
            replaced = TryGrantAlternateItem(player, lootguid, itemId, proto->Quality);

        if (!replaced)
        {
            player->StoreNewItemInBestSlots(itemId, removedCount);

            if (sConfigMgr->GetOption<bool>("ClassLootFilter.Announce", true))
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000[ClassLootFilter]|r Aucun remplacement adapté trouvé, l'objet d'origine vous est rendu.");
        }
    }
};

// Le nom de cette fonction doit correspondre à la convention de votre
// chargeur de modules (basée sur le nom du dossier). Ajuster si besoin,
// comme cela avait déjà été fait pour mod-parangon.
void Addmod_class_loot_filterScripts()
{
    new ClassLootFilter_PlayerScript();
    new ClassLootFilter_GlobalScript();
}