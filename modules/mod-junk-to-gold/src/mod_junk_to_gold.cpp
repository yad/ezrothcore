#include "Chat.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

class JunkToGold : public PlayerScript
{
public:
    JunkToGold() : PlayerScript("JunkToGold") {}

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        if (!item || !item->GetTemplate())
        {
            return;
        }

        if (item->GetTemplate()->Quality == ITEM_QUALITY_POOR)
        {
            SendTransactionInformation(player, item, count);
            player->ModifyMoney(item->GetTemplate()->SellPrice * count);
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        }
    }

private:
    void SendTransactionInformation(Player* player, Item* item, uint32 count)
    {
        std::string itemName = item->GetTemplate()->Name1;
        int loc_idx = player->GetSession() ? player->GetSession()->GetSessionDbLocaleIndex() : DEFAULT_LOCALE;
        if (loc_idx >= 0)
        {
            if (ItemLocale const* il = sObjectMgr->GetItemLocale(item->GetTemplate()->ItemId))
                ObjectMgr::GetLocaleString(il->Name, loc_idx, itemName);
        }

        std::string name;
        if (count > 1)
        {
            name = Acore::StringFormat("|cff9d9d9d|Hitem:{}::::::::80:::::|h[{}]|h|rx{}", item->GetTemplate()->ItemId, itemName, count);
        }
        else
        {
            name = Acore::StringFormat("|cff9d9d9d|Hitem:{}::::::::80:::::|h[{}]|h|r", item->GetTemplate()->ItemId, itemName);
        }

        uint32 money = item->GetTemplate()->SellPrice * count;
        uint32 gold = money / GOLD;
        uint32 silver = (money % GOLD) / SILVER;
        uint32 copper = (money % GOLD) % SILVER;

        std::string info;
        if (money < SILVER)
        {
            info = Acore::StringFormat("{} vendu pour {} cuivre.", name, copper);
        }
        else if (money < GOLD)
        {
            if (copper > 0)
            {
                info = Acore::StringFormat("{} vendu pour {} argent et {} cuivre.", name, silver, copper);
            }
            else
            {
                info = Acore::StringFormat("{} vendu pour {} argent.", name, silver);
            }
        }
        else
        {
            if (copper > 0 && silver > 0)
            {
                info = Acore::StringFormat("{} vendu pour {} or, {} argent et {} cuivre.", name, gold, silver, copper);
            }
            else if (copper > 0)
            {
                info = Acore::StringFormat("{} vendu pour {} or et {} cuivre.", name, gold, copper);
            }
            else if (silver > 0)
            {
                info = Acore::StringFormat("{} vendu pour {} or et {} argent.", name, gold, silver);
            }
            else
            {
                info = Acore::StringFormat("{} vendu pour {} or.", name, gold);
            }
        }

        ChatHandler(player->GetSession()).SendSysMessage(info);
    }
};

void Addmod_junk_to_goldScripts()
{
    new JunkToGold();
}
