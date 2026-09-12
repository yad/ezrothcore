/*
 * mod-class-loot-filter
 * ----------------------
 * Empêche de looter des équipements BoE (Bind on Equip) inutilisables par la
 * classe du joueur (ex: plaques pour un mage) sur les créatures et les coffres.
 *
 * Fallback si aucune pièce "utile" n'est disponible dans la même table de
 * butin :
 *   1) tente de remplacer l'objet par un autre objet du MÊME loot (encore
 *      disponible, non déjà loot) qui EST utilisable par la classe ;
 *   2) sinon, compense en or (basé sur le prix de vente vendeur de l'objet).
 *
 * ATTENTION - à vérifier avant compilation sur votre arbre exact :
 *   - Signature de PlayerScript::OnLootItem (Player*, Item*, uint32, ObjectGuid)
 *   - Membre "loot" sur Creature / GameObject (creature->loot / go->loot)
 *   - Existence de Player::StoreNewItemInBestSlots(uint32 itemId, uint32 count)
 *   - Membres de LootItem (itemid, is_looted, AllowedForPlayer)
 *   - ObjectGuid::IsCreatureOrVehicle() / IsGameObject()
 * Grep ces symboles dans votre core local si une erreur de build apparaît,
 * comme d'habitude, et on corrige au besoin.
 */

#include "ScriptMgr.h"
#include "Player.h"
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

#include <initializer_list>

namespace
{
    // ---------------------------------------------------------------------
    // Table de proficience d'armure par classe (règles classiques WotLK).
    // ---------------------------------------------------------------------
    bool CanWearArmorSubclass(uint8 playerClass, uint32 subclass)
    {
        switch (subclass)
        {
            case ITEM_SUBCLASS_ARMOR_MISC:  // capes, etc. -> jamais restreint
            case ITEM_SUBCLASS_ARMOR_CLOTH: // tissu -> toutes les classes
                return true;

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
                        return false; // mage / prêtre / démoniste : cuir non porté
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
                        return false; // ex: mage -> jamais de plaques
                }

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

    bool IsUsableByClass(ItemTemplate const* proto, uint8 playerClass)
    {
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
            return CanWearArmorSubclass(playerClass, proto->SubClass);
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

    // Cherche, dans le même butin, un autre objet (armure/arme) encore
    // disponible et utilisable par la classe du joueur, pour remplacer
    // l'objet inutile qui vient d'être retiré.
    bool TryGrantAlternateItem(Player* player, ObjectGuid const& lootguid, uint32 excludeItemId)
    {
        Loot* loot = GetLootFromGuid(player, lootguid);
        if (!loot)
            return false;

        for (LootItem& li : loot->items)
        {
            if (li.is_looted || li.itemid == excludeItemId)
                continue;

            ItemTemplate const* altProto = sObjectMgr->GetItemTemplate(li.itemid);
            if (!altProto)
                continue;

            if (altProto->Class != ITEM_CLASS_ARMOR && altProto->Class != ITEM_CLASS_WEAPON)
                continue;

            if (!li.AllowedForPlayer(player, lootguid))
                continue;

            if (!IsUsableByClass(altProto, player->getClass()))
                continue;

            if (player->StoreNewItemInBestSlots(li.itemid, 1))
            {
                li.is_looted = true;

                if (sConfigMgr->GetOption<bool>("ClassLootFilter.Announce", true))
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff8000[ClassLootFilter]|r Objet remplace par une piece adaptee a votre classe.");

                return true;
            }
        }

        return false;
    }

    void GrantGoldFallback(Player* player, ItemTemplate const* proto, uint32 count)
    {
        uint32 pct = sConfigMgr->GetOption<uint32>("ClassLootFilter.GoldCompensationPct", 100);
        uint64 gold = static_cast<uint64>(proto->SellPrice) * count * pct / 100;

        if (gold > 0)
            player->ModifyMoney(static_cast<int64>(gold));

        if (sConfigMgr->GetOption<bool>("ClassLootFilter.Announce", true))
        {
            if (gold > 0)
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000[ClassLootFilter]|r Objet inutilisable par votre classe retire, {} po de compensation.",
                    gold / 10000);
            else
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000[ClassLootFilter]|r Objet inutilisable par votre classe retire.");
        }
    }
}

class ClassLootFilter_PlayerScript : public PlayerScript
{
public:
    ClassLootFilter_PlayerScript() : PlayerScript("ClassLootFilter_PlayerScript") { }

    void OnLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        if (!sConfigMgr->GetOption<bool>("ClassLootFilter.Enable", true))
            return;

        if (!player || !item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;

        // On ne s'occupe que des armures et armes
        if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
            return;

        if (sConfigMgr->GetOption<bool>("ClassLootFilter.OnlyBindOnEquip", true)
            && proto->Bonding != ITEM_BIND_ON_EQUIP)
            return;

        uint32 minIlvl = sConfigMgr->GetOption<uint32>("ClassLootFilter.MinItemLevel", 1);
        if (proto->ItemLevel < minIlvl)
            return;

        if (IsUsableByClass(proto, player->getClass()))
            return; // rien a faire, la piece convient a la classe

        // La piece ne convient pas : on la retire de l'inventaire du joueur
        uint8 bag = item->GetBagSlot();
        uint8 slot = item->GetSlot();
        uint32 itemId = proto->ItemId;
        uint32 removedCount = item->GetCount();

        player->DestroyItem(bag, slot, true);

        bool replaced = false;
        if (sConfigMgr->GetOption<bool>("ClassLootFilter.TryAlternateItem", true))
            replaced = TryGrantAlternateItem(player, lootguid, itemId);

        if (!replaced)
            GrantGoldFallback(player, proto, removedCount);
    }
};

// Le nom de cette fonction doit correspondre à la convention de votre
// chargeur de modules (basée sur le nom du dossier). Ajuster si besoin,
// comme cela avait déjà été fait pour mod-parangon.
void Addmod_class_loot_filterScripts()
{
    new ClassLootFilter_PlayerScript();
}
