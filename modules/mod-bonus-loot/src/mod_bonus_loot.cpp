#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <vector>

namespace
{
    struct BonusLootConfig
    {
        static inline bool Enable = true;
        static inline bool ExcludeBots = true;
        static inline bool StrictArmorPreference = true;
        static inline bool Announce = true;
    };

    bool IsPlayerBot(Player* player)
    {
#ifdef MOD_PLAYERBOTS
        return player && player->GetSession() && player->GetSession()->IsBot();
#else
        return false;
#endif
    }

    bool CanWearArmorSubclass(uint8 playerClass, uint32 subclass)
    {
        switch (subclass)
        {
            case ITEM_SUBCLASS_ARMOR_CLOTH:
                return true;
            case ITEM_SUBCLASS_ARMOR_LEATHER:
                return playerClass == CLASS_ROGUE || playerClass == CLASS_DRUID || playerClass == CLASS_HUNTER
                    || playerClass == CLASS_SHAMAN || playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN
                    || playerClass == CLASS_DEATH_KNIGHT;
            case ITEM_SUBCLASS_ARMOR_MAIL:
                return playerClass == CLASS_HUNTER || playerClass == CLASS_SHAMAN || playerClass == CLASS_WARRIOR
                    || playerClass == CLASS_PALADIN || playerClass == CLASS_DEATH_KNIGHT;
            case ITEM_SUBCLASS_ARMOR_PLATE:
                return playerClass == CLASS_WARRIOR || playerClass == CLASS_PALADIN || playerClass == CLASS_DEATH_KNIGHT;
            default:
                return true;
        }
    }

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
                return player->HasSkill(SKILL_MAIL) ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                return player->HasSkill(SKILL_PLATE_MAIL) ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
            default:
                return ITEM_SUBCLASS_ARMOR_CLOTH;
        }
    }

    bool IsUsefulArmor(ItemTemplate const* proto, Player* player)
    {
        if (!proto || proto->Class != ITEM_CLASS_ARMOR)
            return false;

        if (proto->AllowableClass != -1)
        {
            uint32 classMask = 1u << (player->getClass() - 1);
            if (!(proto->AllowableClass & classMask))
                return false;
        }

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_ARMOR_MISC:
                return true;
            case ITEM_SUBCLASS_ARMOR_SHIELD:
                return player->getClass() == CLASS_WARRIOR || player->getClass() == CLASS_PALADIN
                    || player->getClass() == CLASS_SHAMAN;
            case ITEM_SUBCLASS_ARMOR_LIBRAM:
                return player->getClass() == CLASS_PALADIN;
            case ITEM_SUBCLASS_ARMOR_IDOL:
                return player->getClass() == CLASS_DRUID;
            case ITEM_SUBCLASS_ARMOR_TOTEM:
                return player->getClass() == CLASS_SHAMAN;
            case ITEM_SUBCLASS_ARMOR_SIGIL:
                return player->getClass() == CLASS_DEATH_KNIGHT;
            case ITEM_SUBCLASS_ARMOR_CLOTH:
            case ITEM_SUBCLASS_ARMOR_LEATHER:
            case ITEM_SUBCLASS_ARMOR_MAIL:
            case ITEM_SUBCLASS_ARMOR_PLATE:
                if (!BonusLootConfig::StrictArmorPreference)
                    return CanWearArmorSubclass(player->getClass(), proto->SubClass);
                return proto->SubClass == GetPreferredArmorSubclass(player);
            default:
                return false;
        }
    }

    bool IsBonusQuality(uint32 quality)
    {
        return quality == ITEM_QUALITY_UNCOMMON || quality == ITEM_QUALITY_RARE || quality == ITEM_QUALITY_EPIC;
    }

    bool IsEquipmentItem(ItemTemplate const* proto)
    {
        return proto && proto->InventoryType != INVTYPE_NON_EQUIP
            && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR);
    }

    std::vector<Player*> GetRecipients(Player* looter)
    {
        std::vector<Player*> recipients;
        Group* group = looter->GetGroup();
        if (!group)
        {
            if (!BonusLootConfig::ExcludeBots || !IsPlayerBot(looter))
                recipients.push_back(looter);
            return recipients;
        }

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || member->GetMap() != looter->GetMap())
                continue;
            if (BonusLootConfig::ExcludeBots && IsPlayerBot(member))
                continue;
            recipients.push_back(member);
        }
        return recipients;
    }

    void SendBonusByMail(Player* player, uint32 itemId)
    {
        Item* mailItem = Item::CreateItem(itemId, 1, player);
        if (!mailItem)
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        mailItem->SaveToDB(trans);
        MailDraft("Butin bonus", "Votre inventaire était plein. Votre pièce d'armure bonus vous attend par courrier.")
            .AddItem(mailItem)
            .SendMailTo(trans, MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_CREATURE, 34337));
        CharacterDatabase.CommitTransaction(trans);
    }

    void GiveBonus(Player* player, uint32 itemId)
    {
        if (!player->AddItem(itemId, 1))
            SendBonusByMail(player, itemId);

        if (BonusLootConfig::Announce)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff8000[Maître du jeu]|r Vous recevez une pièce d'armure bonus adaptée à votre classe.");
    }

    LootStore const* GetLootStore(ObjectGuid lootGuid, uint32& lootId, Player* player)
    {
        if (lootGuid.IsCreatureOrVehicle())
        {
            Creature* creature = ObjectAccessor::GetCreature(*player, lootGuid);
            if (creature)
            {
                lootId = creature->GetCreatureTemplate()->lootid;
                return &LootTemplates_Creature;
            }
        }
        else if (lootGuid.IsGameObject())
        {
            GameObject* gameObject = ObjectAccessor::GetGameObject(*player, lootGuid);
            if (gameObject)
            {
                lootId = gameObject->GetGOInfo()->GetLootId();
                return &LootTemplates_Gameobject;
            }
        }

        return nullptr;
    }

    uint32 SelectBonusItem(Player* player, LootStore const* lootStore, uint32 lootId, uint32 quality)
    {
        std::vector<LootStoreItem const*> possibleItems;
        lootStore->CollectPossibleItems(lootId, possibleItems);

        std::vector<uint32> bindOnEquipCandidates;
        std::vector<uint32> fallbackCandidates;
        for (LootStoreItem const* lootItem : possibleItems)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(lootItem->itemid);
            if (!proto || proto->Quality != quality || proto->RequiredLevel > player->GetLevel()
                || !IsUsefulArmor(proto, player))
                continue;

            fallbackCandidates.push_back(proto->ItemId);
            if (proto->Bonding == BIND_WHEN_EQUIPPED)
                bindOnEquipCandidates.push_back(proto->ItemId);
        }

        std::vector<uint32> const& candidates = bindOnEquipCandidates.empty()
            ? fallbackCandidates : bindOnEquipCandidates;
        if (candidates.empty())
            return 0;

        return candidates[urand(0, static_cast<uint32>(candidates.size() - 1))];
    }

    void ProcessBonus(Player* looter, Item* item, ObjectGuid lootGuid)
    {
        if (!BonusLootConfig::Enable || !looter)
            return;

        ItemTemplate const* triggerProto = item ? item->GetTemplate() : nullptr;
        if (!IsEquipmentItem(triggerProto) || !IsBonusQuality(triggerProto->Quality))
            return;

        uint32 lootId = 0;
        LootStore const* lootStore = GetLootStore(lootGuid, lootId, looter);
        if (!lootStore || !lootId)
            return;

        for (Player* recipient : GetRecipients(looter))
        {
            uint32 itemId = SelectBonusItem(recipient, lootStore, lootId, triggerProto->Quality);
            if (itemId)
                GiveBonus(recipient, itemId);
        }
    }
}

class BonusLoot_WorldScript : public WorldScript
{
public:
    BonusLoot_WorldScript() : WorldScript("BonusLoot_WorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        BonusLootConfig::Enable = sConfigMgr->GetOption<bool>("BonusLoot.Enable", true);
        BonusLootConfig::ExcludeBots = sConfigMgr->GetOption<bool>("BonusLoot.ExcludeBots", true);
        BonusLootConfig::StrictArmorPreference = sConfigMgr->GetOption<bool>("BonusLoot.StrictArmorPreference", true);
        BonusLootConfig::Announce = sConfigMgr->GetOption<bool>("BonusLoot.Announce", true);
    }
};

class BonusLoot_PlayerScript : public PlayerScript
{
public:
    BonusLoot_PlayerScript() : PlayerScript("BonusLoot_PlayerScript",
        { PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM }) { }

    // Solo, butin libre, maître du butin, coffres
    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootGuid) override
    {
        ProcessBonus(player, item, lootGuid);
    }

    // Objet attribué à l'issue d'un jet en groupe
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 /*count*/,
        RollVote /*voteType*/, Roll* roll) override
    {
        if (roll && roll->getLoot())
            ProcessBonus(player, item, roll->getLoot()->sourceWorldObjectGUID);
    }
};

void Addmod_bonus_lootScripts()
{
    new BonusLoot_WorldScript();
    new BonusLoot_PlayerScript();
}
