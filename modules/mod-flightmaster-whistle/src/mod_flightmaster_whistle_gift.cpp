#include "Chat.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "Item.h"
#include "Mail.h"
#include "Player.h"
#include "ScriptMgr.h"

constexpr uint32 WHISTLE_ITEM_ID = 70000;   // Sifflet du maître du vol (mod-flightmaster-whistle)
constexpr uint32 POSTMASTER_NPC  = 34337;   // The Postmaster

struct WhistleFlightState : public DataMap::Base
{
    bool wasInFlight = false;
};

class npc_flightmaster_whistle_gift : public PlayerScript
{
public:
    npc_flightmaster_whistle_gift()
        : PlayerScript("npc_flightmaster_whistle_gift", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        WhistleFlightState* state = player->CustomData.GetDefault<WhistleFlightState>("FlightmasterWhistleGift");

        bool isFlying = player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_TAXI_FLIGHT);

        // Transition "au sol" -> "en vol" : c'est le décollage
        if (isFlying && !state->wasInFlight)
            GiveWhistleIfMissing(player);

        state->wasInFlight = isFlying;
    }

private:
    void GiveWhistleIfMissing(Player* player)
    {
        // Vérifie sacs + banque
        if (player->HasItemCount(WHISTLE_ITEM_ID, 1, true))
            return;

        if (player->AddItem(WHISTLE_ITEM_ID, 1))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Le maître du vol vous glisse un sifflet dans la poche. Bon voyage !");
            return;
        }

        // Inventaire plein : envoi par courrier
        Item* mailItem = Item::CreateItem(WHISTLE_ITEM_ID, 1, player);
        if (!mailItem)
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        mailItem->SaveToDB(trans);

        MailDraft(
            "Sifflet du maître du vol",
            "Votre inventaire était plein au moment du décollage.\n"
            "Voici votre sifflet, envoyé par courrier.")
            .AddItem(mailItem)
            .SendMailTo(
                trans,
                MailReceiver(player, player->GetGUID().GetCounter()),
                MailSender(MAIL_CREATURE, POSTMASTER_NPC));

        CharacterDatabase.CommitTransaction(trans);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "Le maître du vol vous offre un sifflet, mais votre inventaire était plein : "
            "il vous a été envoyé par courrier.");
    }
};

void AddNpcFlightmasterWhistleGiftScripts()
{
    new npc_flightmaster_whistle_gift();
}

// void Addmod_flightmaster_whistle_giftScripts()
// {
//     AddNpcFlightmasterWhistleGiftScripts();
// }