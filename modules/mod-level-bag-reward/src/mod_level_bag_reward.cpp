#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Mail.h"
#include "Player.h"
#include "ScriptMgr.h"

constexpr uint32 BAG_ITEM_ID = 23162;      // Onyxia Hide Backpack (36 slots)
constexpr uint32 POSTMASTER_NPC = 34337;   // The Postmaster

class LevelBagReward : public PlayerScript
{
public:
    LevelBagReward() : PlayerScript("LevelBagReward") { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        uint8 newLevel = player->GetLevel();

        // Récompense tous les 10 niveaux
        if (newLevel % 10 != 0)
            return;

        // Essaye d'ajouter le sac directement dans l'inventaire
        if (player->AddItem(BAG_ITEM_ID, 1))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Félicitations pour le niveau %u ! Vous recevez un sac de 36 emplacements.",
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
                "Félicitations pour le niveau %u !\n"
                "Votre inventaire était plein, voici votre sac par courrier.",
                newLevel))
            .AddItem(mailItem)
            .SendMailTo(
                trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_CREATURE, POSTMASTER_NPC));

        CharacterDatabase.CommitTransaction(trans);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "Félicitations pour le niveau %u ! "
            "Votre inventaire était plein, votre sac a été envoyé par courrier.",
            newLevel);
    }
};

void AddLevelBagRewardScripts()
{
    new LevelBagReward();
}