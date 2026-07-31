#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Mail.h"
#include "Player.h"
#include "ScriptMgr.h"

constexpr uint32 POSTMASTER_NPC = 34337;   // The Postmaster

class LevelBagReward : public PlayerScript
{
public:
    LevelBagReward() : PlayerScript("LevelBagReward") { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        uint8 newLevel = player->GetLevel();

        // Récompense tous les 5 niveaux
        if (newLevel % 5 != 0)
            return;

        uint32 BAG_ITEM_ID = 0;

        switch(newLevel) {
            case 5: BAG_ITEM_ID = 4497; break;    //+10   Heavy Brown Bag

            case 10: BAG_ITEM_ID = 10050; break;  //+12   Mageweave Bag

            case 15: BAG_ITEM_ID = 14046; break;  //+14   Runecloth Bag

            case 20: BAG_ITEM_ID = 14155; break;  //+16   Mooncloth Bag
            case 25: BAG_ITEM_ID = 14155; break;  //+16   Mooncloth Bag

            case 30: BAG_ITEM_ID = 21843; break;  //+18   Imbued Netherweave Bag
            case 35: BAG_ITEM_ID = 21843; break;  //+18   Imbued Netherweave Bag

            case 40: BAG_ITEM_ID = 41599; break;  //+20   Frostweave Bag
            case 45: BAG_ITEM_ID = 41599; break;  //+20   Frostweave Bag

            case 50: BAG_ITEM_ID = 41600; break;  //+22   Glacial Bag
            case 55: BAG_ITEM_ID = 41600; break;  //+22   Glacial Bag

            default: BAG_ITEM_ID = 23162; break;  //+36   A Very Large Bag
        }

        // "A Very Large Bag": on limite à 1 exemplaire tous les 10 niveaux
        if (BAG_ITEM_ID == 23162 && newLevel % 10 != 0)
            return;

        // Essaye d'ajouter le sac directement dans l'inventaire
        if (player->AddItem(BAG_ITEM_ID, 1))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Félicitations pour le niveau {} ! Vous recevez un sac de 36 emplacements.",
                newLevel);

            return;
        }

        // Inventaire plein : envoi par courrier
        Item* mailItem = Item::CreateItem(BAG_ITEM_ID, 1, player);
        if (!mailItem)
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        mailItem->SaveToDB(trans);

        MailDraft(
            "Récompense de niveau",
            Acore::StringFormat(
                "Félicitations pour le niveau {} !\n"
                "Votre inventaire était plein, voici votre sac par courrier.",
                newLevel))
            .AddItem(mailItem)
            .SendMailTo(
                trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_CREATURE, POSTMASTER_NPC));

        CharacterDatabase.CommitTransaction(trans);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "Félicitations pour le niveau {} ! "
            "Votre inventaire était plein, votre sac a été envoyé par courrier.",
            newLevel);
    }
};

void AddLevelBagRewardScripts()
{
    new LevelBagReward();
}

void Addmod_level_bag_rewardScripts()
{
    AddLevelBagRewardScripts();
}