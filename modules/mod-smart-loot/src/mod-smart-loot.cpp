/*
 * mod-smart-loot
 * --------------
 * Deux comportements DISTINCTS :
 *
 * 1) BoE (Bind on Equip) sur créatures/coffres, tous types de mobs :
 *    au moment du loot (PlayerScript::OnLootItem), si l'objet n'est pas
 *    utilisable/préféré pour la classe du joueur, il est retiré, puis :
 *      a) tente de le remplacer par un autre objet du MÊME loot encore
 *         disponible qui EST utilisable ;
 *      b) sinon, cherche un objet compatible dans la table de loot possible
 *         du coffre ;
 *      c) sinon, rend l'objet d'origine.
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
#include <sstream>
#include <vector>
#include <algorithm>

namespace
{
    struct SmartLootConfig
    {
        static inline bool Enable = true;
        static inline bool ExcludeBots = true;
        static inline bool OnlyBosses = false;
        static inline bool SmartLootEnable = true;
        static inline float SmartLootBoostFactor = 2.0f;
        static inline float SmartLootPenaltyFactor = 0.3f;
        static inline bool OnlyBindOnEquip = true;
        static inline bool FilterArmor = true;
        static inline bool StrictArmorPreference = true;
        static inline bool FilterWeapons = false;
        static inline uint32 MinItemLevel = 1;
        static inline bool TryAlternateItem = true;
        static inline bool Announce = true;
    };

    // ---------------------------------------------------------------------
    // Ancienne table "permissive" (tout ce que la classe PEUT porter).
    // Conservée pour SmartLoot.StrictArmorPreference = 0.
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
                if (!SmartLootConfig::StrictArmorPreference)
                    return CanWearArmorSubclass(playerClass, subclass);
                return subclass == GetPreferredArmorSubclass(player);

            default:
                return true; // sous-classe inconnue -> ne pas bloquer par prudence
        }
    }

    // ---------------------------------------------------------------------
    // Table de proficience d'armes par classe (approximatif, WotLK classique).
    // Désactivée par défaut (SmartLoot.FilterWeapons = 0) : à activer
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
            if (!SmartLootConfig::FilterArmor)
                return true;

            if (proto->InventoryType == INVTYPE_CLOAK)
                return true;

            return IsArmorSubclassAccepted(player, proto->SubClass);
        }

        if (proto->Class == ITEM_CLASS_WEAPON)
        {
            if (!SmartLootConfig::FilterWeapons)
                return true;
            return CanUseWeaponSubclass(playerClass, proto->SubClass);
        }

        return true;
    }

    std::string GetItemLink(uint32 itemId, Player* player)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return "";

        int localeIndex = player->GetSession()->GetSessionDbLocaleIndex();
        std::string name = proto->Name1;
        if (ItemLocale const* itemLocale = sObjectMgr->GetItemLocale(itemId))
            ObjectMgr::GetLocaleString(itemLocale->Name, localeIndex, name);

        std::ostringstream itemLink;
        itemLink << "|c" << std::hex << ItemQualityColors[proto->Quality] << std::dec
            << "|Hitem:" << itemId << ":0:0:0:0:0:0:0:0:0|h[" << name << "]|h|r";
        return itemLink.str();
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
        bool excludeBots = SmartLootConfig::ExcludeBots;

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

    // Cherche d'abord dans le loot généré, puis dans la table possible, un
    // autre objet (armure/arme) de même qualité utilisable par le joueur.
    bool TryGrantAlternateItem(Player* player, ObjectGuid const& lootguid, uint32 excludeItemId,
        uint32 requiredQuality)
    {
        Loot* loot = GetLootFromGuid(player, lootguid);
        if (!loot)
            return false;

        struct LootCandidate
        {
            LootItem* item;
            bool fromTemplate;
        };

        std::vector<LootCandidate> candidates;
        std::vector<LootItem> templateItems;
        ObjectGuid sourceGuid = loot->sourceWorldObjectGUID ? loot->sourceWorldObjectGUID : lootguid;

        auto collectCandidates = [&](std::vector<LootItem>& items)
        {
            for (size_t index = 0; index < items.size(); ++index)
            {
                LootItem& li = items[index];
                if (li.is_looted || li.is_blocked || li.freeforall || li.needs_quest || !li.conditions.empty()
                    || li.itemid == excludeItemId)
                    continue;

                if (li.rollWinnerGUID && li.rollWinnerGUID != player->GetGUID())
                    continue;

                if (loot->roundRobinPlayer && loot->roundRobinPlayer != player->GetGUID() && li.is_underthreshold)
                    continue;

                ItemTemplate const* altProto = sObjectMgr->GetItemTemplate(li.itemid);
                if (!altProto)
                    continue;

                if (altProto->Class != ITEM_CLASS_ARMOR && altProto->Class != ITEM_CLASS_WEAPON)
                    continue;

                if (altProto->Quality != requiredQuality)
                    continue;

                if (!li.AllowedForPlayer(player, sourceGuid))
                    continue;

                if (!IsUsableByClass(altProto, player))
                    continue;

                candidates.push_back({ &li, false });
            }
        };

        collectCandidates(loot->items);
        collectCandidates(loot->quest_items);

        LootStore const* lootStore = nullptr;
        uint32 lootId = 0;
        if (lootguid.IsCreatureOrVehicle())
        {
            if (Creature* creature = ObjectAccessor::GetCreature(*player, lootguid))
            {
                lootStore = &LootTemplates_Creature;
                lootId = creature->GetCreatureTemplate()->lootid;
            }
        }
        else if (lootguid.IsGameObject())
        {
            if (GameObject* go = ObjectAccessor::GetGameObject(*player, lootguid))
            {
                lootStore = &LootTemplates_Gameobject;
                lootId = go->GetGOInfo()->GetLootId();
            }
        }

        if (candidates.empty() && lootStore && lootId)
        {
            std::vector<LootStoreItem const*> possibleItems;
            lootStore->CollectPossibleItems(lootId, possibleItems);
            templateItems.reserve(possibleItems.size());

            for (LootStoreItem const* possibleItem : possibleItems)
            {
                if (!possibleItem || possibleItem->itemid == excludeItemId)
                    continue;

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(possibleItem->itemid);
                if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
                    continue;

                if (proto->Quality != requiredQuality || !IsUsableByClass(proto, player))
                    continue;

                templateItems.emplace_back(*possibleItem);
                LootItem& candidate = templateItems.back();
                candidate.count = std::max<uint8>(1, possibleItem->mincount);
                if (!candidate.AllowedForPlayer(player, sourceGuid))
                {
                    templateItems.pop_back();
                    continue;
                }

                candidates.push_back({ &candidate, true });
            }
        }

        if (candidates.empty())
            return false;

        LootCandidate selected = candidates[urand(0, static_cast<uint32>(candidates.size() - 1))];
        LootItem* selectedItem = selected.item;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, selectedItem->itemid,
            selectedItem->count) == EQUIP_ERR_OK)
        {
            AllowedLooterSet looters = selectedItem->GetAllowedLooters();
            Item* replacement = player->StoreNewItem(dest, selectedItem->itemid, true,
                selectedItem->randomPropertyId, looters);
            if (!replacement)
                return false;

            if (!selected.fromTemplate)
            {
                selectedItem->is_looted = true;
                if (loot->unlootedCount > 0)
                    --loot->unlootedCount;
                loot->NotifyItemRemoved(selectedItem->itemIndex);
            }

            std::string replacementLink = GetItemLink(selectedItem->itemid, player);
            if (SmartLootConfig::Announce)
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff8000[Maître du Jeu]|r Objet remplacé par {}, une pièce adaptée à votre classe.",
                    replacementLink);

            return true;
        }

        return false;
    }

}

class SmartLoot_WorldScript : public WorldScript
{
public:
    SmartLoot_WorldScript() : WorldScript("SmartLoot_WorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        SmartLootConfig::Enable = sConfigMgr->GetOption<bool>("SmartLoot.Enable", true);
        SmartLootConfig::ExcludeBots = sConfigMgr->GetOption<bool>("SmartLoot.ExcludeBots", true);
        SmartLootConfig::OnlyBosses = sConfigMgr->GetOption<bool>("SmartLoot.OnlyBosses", false);
        SmartLootConfig::SmartLootEnable = sConfigMgr->GetOption<bool>("SmartLoot.SmartLootEnable", true);
        SmartLootConfig::SmartLootBoostFactor = std::max(0.0f,
            sConfigMgr->GetOption<float>("SmartLoot.SmartLootBoostFactor", 2.0f));
        SmartLootConfig::SmartLootPenaltyFactor = std::max(0.0f,
            sConfigMgr->GetOption<float>("SmartLoot.SmartLootPenaltyFactor", 0.3f));
        SmartLootConfig::OnlyBindOnEquip = sConfigMgr->GetOption<bool>("SmartLoot.OnlyBindOnEquip", true);
        SmartLootConfig::FilterArmor = sConfigMgr->GetOption<bool>("SmartLoot.FilterArmor", true);
        SmartLootConfig::StrictArmorPreference = sConfigMgr->GetOption<bool>("SmartLoot.StrictArmorPreference", true);
        SmartLootConfig::FilterWeapons = sConfigMgr->GetOption<bool>("SmartLoot.FilterWeapons", false);
        SmartLootConfig::MinItemLevel = sConfigMgr->GetOption<uint32>("SmartLoot.MinItemLevel", 1);
        SmartLootConfig::TryAlternateItem = sConfigMgr->GetOption<bool>("SmartLoot.TryAlternateItem", true);
        SmartLootConfig::Announce = sConfigMgr->GetOption<bool>("SmartLoot.Announce", true);
    }
};

class SmartLoot_GlobalScript : public GlobalScript
{
public:
    SmartLoot_GlobalScript() : GlobalScript("SmartLoot_GlobalScript", {
        GLOBALHOOK_ON_ITEM_ROLL
    }) { }

    // "Smart loot" : pondère la chance de drop des pièces SOULBOUND (BoP)
    // sur les boss de donjon/raid selon les classes des joueurs humains du
    // groupe. Ne touche jamais au BoE (géré par OnLootItem) ni aux boss de
    // monde ouvert / trashs.
    bool OnItemRoll(Player const* player, LootStoreItem const* lootStoreItem, float& chance, Loot& loot,
        LootStore const& /*store*/) override
    {
        if (!SmartLootConfig::SmartLootEnable)
            return true;

        if (!player || !lootStoreItem)
            return true;

        if (loot.sourceGameObject)
            return true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(lootStoreItem->itemid);
        if (!proto)
            return true;

        // Uniquement armures/armes soulbound (BoP) - le BoE reste géré par OnLootItem
        if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
            return true;

        if (proto->Bonding != BIND_WHEN_PICKED_UP)
            return true;

        if (!loot.sourceWorldObjectGUID.IsCreatureOrVehicle())
            return true;

        Player* nonConstPlayer = const_cast<Player*>(player);

        Creature* creature = ObjectAccessor::GetCreature(*nonConstPlayer, loot.sourceWorldObjectGUID);
        if (!creature || !creature->IsDungeonBoss())
            return true; // uniquement les boss de donjon/raid

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
            ? SmartLootConfig::SmartLootBoostFactor
            : SmartLootConfig::SmartLootPenaltyFactor;

        chance = std::max(0.0f, std::min(chance * factor, 100.0f));

        return true;
    }
};

class SmartLoot_PlayerScript : public PlayerScript
{
public:
    SmartLoot_PlayerScript() : PlayerScript("SmartLoot_PlayerScript", {
        PLAYERHOOK_ON_LOOT_ITEM
    }) { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
    {
        static thread_local bool replacementInProgress = false;
        if (replacementInProgress)
            return;

        if (!SmartLootConfig::Enable)
            return;

        if (!player || !item)
            return;

        if (SmartLootConfig::ExcludeBots && IsPlayerBot(player))
            return; // on ne touche pas au loot des playerbots

        bool isBossLoot = IsDungeonOrRaidBossLoot(player, lootguid);

        if (SmartLootConfig::OnlyBosses && !isBossLoot)
            return; // mode restreint aux boss de donjon/raid uniquement

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;

        if (count != 1 || proto->GetMaxStackSize() != 1)
            return;

        // On ne s'occupe que des armures et armes
        if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
            return;

        if (SmartLootConfig::OnlyBindOnEquip
            && proto->Bonding != BIND_WHEN_EQUIPPED)
            return;

        uint32 minIlvl = SmartLootConfig::MinItemLevel;
        if (proto->ItemLevel < minIlvl)
            return;

        if (IsUsableByClass(proto, player))
            return; // rien a faire, la piece convient a la classe

        // La piece ne convient pas : on ne la retire qu'apres avoir stocke son remplacement.
        uint32 itemId = proto->ItemId;
        std::string itemLink = GetItemLink(itemId, player);

        struct ReplacementGuard
        {
            bool& active;

            explicit ReplacementGuard(bool& active) : active(active) { active = true; }
            ~ReplacementGuard() { active = false; }
        } replacementGuard(replacementInProgress);

        bool replaced = false;
        if (SmartLootConfig::TryAlternateItem)
            replaced = TryGrantAlternateItem(player, lootguid, itemId, proto->Quality);

        if (replaced)
        {
            uint32 countToRemove = count;
            player->DestroyItemCount(item, countToRemove, true);
        }
        else if (SmartLootConfig::Announce)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff8000[Maître du Jeu]|r Aucun remplacement adapté trouvé, {} vous est conservé.", itemLink);
        }
    }
};

// Le nom de cette fonction doit correspondre à la convention de votre
// chargeur de modules (basée sur le nom du dossier). Ajuster si besoin,
// comme cela avait déjà été fait pour mod-parangon.
void Addmod_smart_lootScripts()
{
    new SmartLoot_WorldScript();
    new SmartLoot_PlayerScript();
    new SmartLoot_GlobalScript();
}