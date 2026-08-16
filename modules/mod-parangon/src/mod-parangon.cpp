/*
 * mod-parangon
 *
 * Systeme de niveau "parangon" : au-dela d'un seuil parametrable, le level
 * up classique est remplace par une progression de stats brutes (+X%/palier),
 * tout en neutralisant l'ecart de puissance contre les monstres sous ce seuil
 * afin que le combat reste equivalent a un affrontement seuil vs seuil.
 *
 * STATUT : squelette de depart. Voir les blocs TODO ci-dessous et le README
 * du module avant compilation. Les signatures de hooks (OnGiveXP,
 * ModifyMeleeDamage, ModifySpellDamageTaken, GetPlayerSetting/
 * UpdatePlayerSetting, ...) sont a verifier contre la version exacte du core
 * utilisee (elles evoluent entre revisions AC).
 *
 * Design cle : aucune table SQL custom, aucune extension de
 * player_xp_for_level. L'XP et le leveling au-dela du seuil sont geres
 * entierement par ce module :
 *   - le coeur ne voit jamais amount > 0 une fois le seuil atteint (on met
 *     amount a 0 dans OnGiveXP pour empecher sa propre boucle de level up,
 *     qui depend de player_xp_for_level et planterait/boclerait au-dela des
 *     niveaux connus du coeur) ;
 *   - le module accumule sa propre XP et appelle player->GiveLevel() lui-
 *     meme quand le seuil dynamique (calcule en C++, formule geometrique)
 *     est atteint ;
 *   - la persistance du compteur d'XP parangon utilise le mecanisme
 *     generique "player setting" deja fourni par AzerothCore
 *     (table character_settings existante du schema core, pas une table
 *     ajoutee par ce module) plutot qu'un champ custom.
 */

#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Unit.h"
#include "Creature.h"
#include "Formulas.h"    // Acore::XP::BaseGain, Acore::XP::xp_in_group_rate
#include "DBCStores.h"   // GetContentLevelsForMapAndZone - a verifier selon la version du core
#include "Group.h"
#include "World.h"       // sWorld->getRate(RATE_XP_KILL)
#include "LootMgr.h"     // struct Loot (OnPlayerBeforeLootMoney)
#include "ObjectAccessor.h" // ObjectAccessor::GetCreature
#include "ObjectMgr.h"   // sObjectMgr->GetXPForLevel (pont de niveau sous le seuil)
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "CellImpl.h"
#include "CreatureAI.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <mutex>

namespace ParangonConfig
{
    bool   Enabled = true;
    uint32 Threshold = 60;
    uint32 MaxLevel = 250;
    float  StatsPerLevelPct = 1.0f;   // % de bonus de stats par palier
    float  DamageEquityFactor = 1.0f;   // ajustement fin de la neutralisation
    uint32 XPBaseRequirement = 50000;  // XP requise pour le 1er palier parangon
    float  XPGrowthPct = 5.0f;   // croissance en % de l'XP requise par palier
    float  QuestXPCapPct = 5.0f;   // plafond de l'XP de quete, en % de l'XP requise pour le palier courant
    bool   ForceAggroEnabled = true;   // force l'engagement des monstres hostiles sous le seuil a proximite
    float  ForceAggroRangeYards = 20.0f; // portee fixe utilisee a la place de la portee native (qui depend du niveau reel)
    uint32 ForceAggroCheckIntervalMs = 1000; // frequence du scan de proximite, par joueur
    float  CombatEquityPctPerLevel = 3.0f;   // % de degats en plus/moins par niveau d'ecart sous le seuil (compose), applique symetriquement dans les deux sens
    float  CombatEquityMaxFactor = 5.0f;     // plafond du multiplicateur, pour eviter un one-shot ou des degats nuls sur gros ecart
    bool   BypassProgressionXPBlock = true;  // contourne un blocage externe de l'XP sous le seuil (ex: mod-individual-progression) pour les kills, en gerant nous-memes PLAYER_XP
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static bool IsParangon(Player* player)
{
    return player && player->GetLevel() >= ParangonConfig::Threshold;
}

// Nombre de paliers parangon au-dessus du seuil (0 si pas encore parangon).
static uint32 GetParangonLevel(Player* player)
{
    if (!IsParangon(player))
        return 0;

    return player->GetLevel() - ParangonConfig::Threshold;
}

// Resout le joueur parangon "controlant" une unite : soit l'unite EST le
// joueur, soit c'est un familier/garde dont le proprietaire est ce joueur.
// Permet a l'equite de combat de couvrir aussi les familiers, pas
// uniquement les coups portes/recus directement par le joueur.
static Player* GetControllingParangonPlayer(Unit* unit)
{
    if (!unit)
        return nullptr;

    if (unit->GetTypeId() == TYPEID_PLAYER)
        return IsParangon(unit->ToPlayer()) ? unit->ToPlayer() : nullptr;

    // TODO: verifier que GetOwner() est bien le moyen correct de retrouver
    // le proprietaire d'un familier/garde sur cette version du core
    // (parfois GetCharmerOrOwner() selon les cas de charme/possession).
    if (Unit* owner = unit->GetOwner())
    {
        if (owner->GetTypeId() == TYPEID_PLAYER)
            return IsParangon(owner->ToPlayer()) ? owner->ToPlayer() : nullptr;
    }

    return nullptr;
}

// Facteur multiplicatif de stats accumule par les paliers parangon.
// ex: 20 paliers x 1% = 1.20f
static float GetParangonStatFactor(Player* player)
{
    uint32 parangonLevel = GetParangonLevel(player);
    return 1.0f + (parangonLevel * ParangonConfig::StatsPerLevelPct / 100.0f);
}

// Facteur d'equite de combat pour un monstre sous le seuil, base sur
// l'ecart de niveau reel avec le seuil (pas sur le palier parangon du
// joueur). Utilise SYMETRIQUEMENT dans les deux sens par
// ApplyEquityFactorUnsigned : diviseur pour les degats infliges par le
// joueur/familier AU monstre, multiplicateur pour les degats infliges PAR
// le monstre au joueur/familier. Ne modifie jamais les stats reelles du
// monstre - uniquement le nombre de degats deja calcule par le moteur,
// apres resolution du coup. Croissance composee configurable par niveau
// d'ecart, plafonnee pour eviter un one-shot ou des degats nuls sur un
// tres gros ecart (mob niveau 1 vs joueur niveau 200).
static float GetCombatEquityFactor(Creature* mob)
{
    if (!mob || mob->GetLevel() >= ParangonConfig::Threshold)
        return 1.0f;

    uint32 levelGap = ParangonConfig::Threshold - mob->GetLevel();
    float factor = std::pow(1.0f + ParangonConfig::CombatEquityPctPerLevel / 100.0f, static_cast<float>(levelGap));
    return std::min(factor, ParangonConfig::CombatEquityMaxFactor);
}

// XP requise pour passer du palier parangonLevel au palier parangonLevel+1.
// Formule geometrique calculee entierement en C++, aucune lecture DB.
static uint32 GetParangonXPRequired(uint32 parangonLevel)
{
    float growth = 1.0f + (ParangonConfig::XPGrowthPct / 100.0f);
    double required = static_cast<double>(ParangonConfig::XPBaseRequirement) * std::pow(growth, static_cast<double>(parangonLevel));
    return static_cast<uint32>(required);
}

// Reapplique manuellement ce que Acore::XP::Gain() ajoute normalement par-
// dessus BaseGain() (puisqu'on n'appelle jamais Gain() directement dans ce
// module) : bonus elite/worldboss (x2 en monde ouvert, x2.75 en donjon,
// comportement Blizzlike documente), Rate.XP.Kill, bonus de groupe.
//
// TODO: ce bloc reconstitue la logique connue de Acore::XP::Gain() a partir
// du comportement Blizzlike documente (Formulas.cpp n'expose pas
// directement Gain() en tant que fonction reutilisable telle quelle) - a
// comparer ligne a ligne avec Formulas.cpp de ta version du core pour
// confirmer les coefficients exacts (2.0f / 2.75f) et l'ordre d'application.
static uint32 ApplyKillXPMultipliers(Player* player, Unit* victim, uint32 gain)
{
    if (gain == 0)
        return 0;

    if (Creature* creature = victim->ToCreature())
    {
        bool isEliteOrBoss = creature->isElite() || creature->isWorldBoss();
        if (isEliteOrBoss)
        {
            float eliteBonus = creature->GetMap() && creature->GetMap()->IsDungeon() ? 2.75f : 2.0f;
            gain = static_cast<uint32>(gain * eliteBonus);
        }
    }

    gain = static_cast<uint32>(gain * sWorld->getRate(RATE_XP_KILL));

    if (Group* group = player->GetGroup())
    {
        uint32 memberCount = group->GetMembersCount();
        gain = static_cast<uint32>(gain * Acore::XP::xp_in_group_rate(memberCount, group->isRaidGroup()));
    }

    return gain;
}

// Calcule le gain d'XP "symetrique" en reutilisant la formule native du
// coeur, mais en forcant le "niveau de la victime" au niveau du joueur
// plutot que son niveau reel (qui donnerait normalement un gain nul, un
// monstre sous le seuil parangon etant "gris").
//
// NOTE IMPORTANTE : dans AzerothCore (contrairement a TrinityCore dont il
// derive), le namespace a ete renomme Acore::XP (et non plus Trinity::XP).
// C'est Acore::XP::BaseGain(pl_level, mob_level, content) qu'on appelle
// directement plutot que Acore::XP::Gain(player, unit) - Gain() lit lui-meme
// le niveau reel de la victime via unit->GetLevel() en interne, donc
// l'appeler directement ne permettrait pas de forcer le niveau. BaseGain()
// est la fonction de plus bas niveau, sans cette lecture implicite.
static uint32 ComputeParangonXPGain(Player* player, Unit* victim)
{
    if (!player || !victim)
        return 0;

    // Memes exclusions que Acore::XP::Gain() : totems, familiers, critters
    // ne donnent pas d'XP.
    if (victim->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = victim->ToCreature();
        if (creature && (creature->IsTotem() || creature->IsPet() || creature->IsCritter()))
            return 0;
    }

    uint8 level = player->GetLevel();

    // TODO: verifier la signature exacte de GetContentLevelsForMapAndZone
    // sur ta version du core (fonction libre declaree pres de DBCStores,
    // parfois exposee via sObjectMgr selon les revisions).
    ContentLevels content = GetContentLevelsForMapAndZone(player->GetMapId(), player->GetZoneId());

    // Niveau de victime force au niveau du joueur : c'est ce qui rend le
    // gain "symetrique" au lieu de refleter l'ecart de niveau reel.
    uint32 gain = Acore::XP::BaseGain(level, level, content);
    return ApplyKillXPMultipliers(player, victim, gain);
}

// Variante "niveaux reels" (pas de niveau force) de ComputeParangonXPGain,
// utilisee uniquement pour le pont de niveau sous le seuil parangon
// (Parangon.BypassProgressionXPBlock) : reproduit un gain d'XP natif
// normal, comme si aucun autre module ne bloquait "amount" dans
// OnPlayerGiveXP.
static uint32 ComputeNativeKillXPGain(Player* player, Unit* victim)
{
    if (!player || !victim)
        return 0;

    if (victim->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = victim->ToCreature();
        if (creature && (creature->IsTotem() || creature->IsPet() || creature->IsCritter()))
            return 0;
    }

    ContentLevels content = GetContentLevelsForMapAndZone(player->GetMapId(), player->GetZoneId());
    uint32 gain = Acore::XP::BaseGain(player->GetLevel(), victim->GetLevel(), content);
    return ApplyKillXPMultipliers(player, victim, gain);
}

// ---------------------------------------------------------------------
// WorldScript : chargement de la config
// ---------------------------------------------------------------------

class Parangon_WorldScript : public WorldScript
{
public:
    // NOTE: contrairement a PlayerScript, WorldScript ne semble pas avoir
    // renomme ses methodes (OnAfterConfigLoad reste OnAfterConfigLoad), mais
    // le meme systeme de declaration des hooks dans le constructeur existe
    // desormais. Ajoute ici par coherence/precaution ; a retirer si ton
    // arbre accepte encore l'ancien constructeur a un seul argument.
    Parangon_WorldScript() : WorldScript("Parangon_WorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
        }) {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        ParangonConfig::Enabled = sConfigMgr->GetOption<bool>("Parangon.Enable", true);
        ParangonConfig::Threshold = sConfigMgr->GetOption<uint32>("Parangon.Threshold", 60);
        ParangonConfig::MaxLevel = sConfigMgr->GetOption<uint32>("Parangon.MaxLevel", 250);
        ParangonConfig::StatsPerLevelPct = sConfigMgr->GetOption<float>("Parangon.StatsPerLevelPct", 1.0f);
        ParangonConfig::DamageEquityFactor = sConfigMgr->GetOption<float>("Parangon.DamageEquityFactor", 1.0f);
        ParangonConfig::XPBaseRequirement = sConfigMgr->GetOption<uint32>("Parangon.XPBaseRequirement", 50000);
        ParangonConfig::XPGrowthPct = sConfigMgr->GetOption<float>("Parangon.XPGrowthPct", 5.0f);
        ParangonConfig::QuestXPCapPct = sConfigMgr->GetOption<float>("Parangon.QuestXPCapPct", 5.0f);
        ParangonConfig::ForceAggroEnabled = sConfigMgr->GetOption<bool>("Parangon.ForceAggroEnabled", true);
        ParangonConfig::ForceAggroRangeYards = sConfigMgr->GetOption<float>("Parangon.ForceAggroRangeYards", 20.0f);
        ParangonConfig::ForceAggroCheckIntervalMs = sConfigMgr->GetOption<uint32>("Parangon.ForceAggroCheckIntervalMs", 1000);
        ParangonConfig::CombatEquityPctPerLevel = sConfigMgr->GetOption<float>("Parangon.CombatEquityPctPerLevel", 3.0f);
        ParangonConfig::CombatEquityMaxFactor = sConfigMgr->GetOption<float>("Parangon.CombatEquityMaxFactor", 5.0f);
        ParangonConfig::BypassProgressionXPBlock = sConfigMgr->GetOption<bool>("Parangon.BypassProgressionXPBlock", true);
    }
};

// ---------------------------------------------------------------------
// PlayerScript : bonus de stats + XP/leveling geres en C++ au-dela du seuil
// ---------------------------------------------------------------------

class Parangon_PlayerScript : public PlayerScript
{
public:
    // Le constructeur doit desormais declarer explicitement les hooks
    // utilises (systeme de filtrage d'evenements introduit par une revision
    // recente d'AzerothCore). Les methodes elles-memes sont renommees avec
    // le prefixe "Player" (OnLogin -> OnPlayerLogin, etc.) : c'est la cause
    // des erreurs C3668 rencontrees a la compilation.
    Parangon_PlayerScript() : PlayerScript("Parangon_PlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_CREATURE_KILLED_BY_PET
        }) {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!ParangonConfig::Enabled)
            return;

        // Etat frais a la connexion (les modificateurs UnitMods ne sont pas
        // persistes en DB, ils sont reconstruits a chaque chargement) : on
        // applique directement le bonus courant, sans retrait prealable.
        uint32 parangonLevel = GetParangonLevel(player);
        if (parangonLevel > 0)
            ApplyParangonStatModifier(player, parangonLevel, true);

        if (IsParangon(player))
        {
            // PLAYER_XP est deja charge/persiste nativement (sauvegarde de
            // personnage standard) : pas besoin de le relire depuis un
            // stockage separe. Seul PLAYER_NEXT_LEVEL_XP doit etre corrige,
            // car le coeur le recalcule potentiellement au chargement via
            // sa propre table (non etendue au-dela du seuil).
            uint32 required = GetParangonXPRequired(parangonLevel);
            player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, required);
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!ParangonConfig::Enabled)
            return;

        uint32 oldParangonLevel = (oldLevel >= ParangonConfig::Threshold) ? (oldLevel - ParangonConfig::Threshold) : 0;
        uint32 newParangonLevel = GetParangonLevel(player);

        if (oldParangonLevel == newParangonLevel)
            return;

        // Retire le bonus correspondant a l'ancien palier avant d'appliquer
        // celui du nouveau, pour eviter tout cumul incorrect du pourcentage.
        if (oldParangonLevel > 0)
            ApplyParangonStatModifier(player, oldParangonLevel, false);

        if (newParangonLevel > 0)
            ApplyParangonStatModifier(player, newParangonLevel, true);
    }

    // Signature confirmee par le compilateur (4 parametres, avec un uint8
    // en 4e position correspondant a PlayerXPSource - enum du core avec les
    // valeurs XPSOURCE_KILL, XPSOURCE_QUEST, XPSOURCE_QUEST_DF,
    // XPSOURCE_EXPLORE, XPSOURCE_BATTLEGROUND).
    //
    // XPSOURCE_KILL n'est PAS traite ici : pour un kill "gris" (tres bas
    // niveau), le coeur calcule un XP natif nul en amont et ne semble pas
    // appeler GiveXP() du tout dans ce cas - ce hook ne se declenche donc
    // jamais pour ces kills-la. La gestion du kill est deplacee vers
    // OnPlayerCreatureKill, qui se declenche pour TOUT kill independamment
    // du montant d'XP natif.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 xpSource) override
    {
        if (!ParangonConfig::Enabled || !IsParangon(player))
            return; // sous le seuil : laisse le coeur gerer nativement

        if (xpSource == XPSOURCE_KILL)
        {
            // Empeche quand meme le coeur d'utiliser sa propre boucle de
            // level up si jamais amount > 0 pour ce kill precis (cas ou le
            // kill n'etait pas "gris"). Le vrai gain est deja gere via
            // OnPlayerCreatureKill.
            amount = 0;
            return;
        }

        uint32 gain = 0;
        bool isQuestSource = (xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF);

        if (isQuestSource)
        {
            // XP de quete : le coeur a deja calcule un montant specifique a
            // la quete dans "amount" (formule native, qui compare le niveau
            // reel-tres-eleve du joueur au niveau de la quete et le reduit
            // donc fortement). On reprend ce montant tel quel plutot que de
            // le recalculer, mais on le plafonne a Parangon.QuestXPCapPct %
            // de l'XP requise pour le palier parangon courant - meme
            // principe que l'ancien systeme MaNGOS (limite de 5% pour
            // eviter qu'une seule quete ne fasse gagner un palier entier
            // d'un coup).
            uint32 required = GetParangonXPRequired(GetParangonLevel(player));
            uint32 maxQuestGain = static_cast<uint32>(required * (ParangonConfig::QuestXPCapPct / 100.0f));
            gain = std::min(amount, maxQuestGain);
        }
        // XPSOURCE_EXPLORE / XPSOURCE_BATTLEGROUND : ni kill ni quete, gain
        // laisse a 0 pour l'instant (a decider si on veut aussi les couvrir).

        // Empeche le coeur d'utiliser sa propre boucle de level up (qui
        // s'appuie sur player_xp_for_level, non etendue au-dela du seuil).
        amount = 0;

        if (gain > 0)
            GrantParangonXP(player, gain);
    }

    // Se declenche pour TOUT kill de creature, independamment du montant
    // d'XP natif calcule (contrairement a OnPlayerGiveXP pour la source
    // XPSOURCE_KILL, cf. commentaire ci-dessus). C'est ici que se fait
    // reellement le gain d'XP pour un kill, y compris les kills "gris".
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        HandleCreatureKillXP(killer, killed, "OnPlayerCreatureKill");
    }

    // Cas ou c'est le FAMILIER du joueur qui porte le coup fatal (chasseur,
    // demoniste, etc.) : le coeur declenche ce hook distinct plutot que
    // OnPlayerCreatureKill. Sans cette prise en charge, un joueur qui joue
    // principalement via son familier ne gagnerait jamais d'XP parangon.
    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* killed) override
    {
        HandleCreatureKillXP(petOwner, killed, "OnPlayerCreatureKilledByPet");
    }

private:
    void HandleCreatureKillXP(Player* killer, Creature* killed, char const* source)
    {
        if (!ParangonConfig::Enabled || !killer || !killed)
            return;

        // Log d'entree systematique, avant tout branchement - permet de
        // confirmer que le hook se declenche bien pour CE joueur precis,
        // quel que soit le chemin emprunte ensuite.
        LOG_INFO("module", "[mod-parangon] {} ENTREE {} (lvl {}, IsParangon={}, BypassEnabled={}) vs {} (lvl {})",
            source, killer->GetName(), killer->GetLevel(), IsParangon(killer), ParangonConfig::BypassProgressionXPBlock,
            killed->GetName(), killed->GetLevel());

        if (IsParangon(killer))
        {
            uint32 gain = ComputeParangonXPGain(killer, killed);

            LOG_INFO("module", "[mod-parangon] branche PARANGON {} (lvl {}) vs {} (lvl {}, elite={}, critter={}) -> gain={}, RateXPKill={}",
                killer->GetName(), killer->GetLevel(), killed->GetName(), killed->GetLevel(),
                killed->isElite(), killed->IsCritter(), gain, sWorld->getRate(RATE_XP_KILL));

            if (gain > 0)
                GrantParangonXP(killer, gain);
            return;
        }

        // Sous le seuil : ce module ne s'occupe normalement de rien (le
        // coeur gere nativement). Exception si BypassProgressionXPBlock est
        // active - dans ce cas on gere nous-memes le gain "pont" vers le
        // seuil, independamment de ce qu'un autre module (ex:
        // mod-individual-progression) a pu faire a "amount" dans
        // OnPlayerGiveXP - ce hook-ci n'utilise pas "amount" du tout, donc
        // aucun blocage externe sur ce champ ne peut nous affecter ici.
        if (ParangonConfig::BypassProgressionXPBlock)
            GrantBridgeXP(killer, killed);
        else
            LOG_INFO("module", "[mod-parangon] branche PONT ignoree (BypassProgressionXPBlock=0) pour {}", killer->GetName());
    }

    // Reduit l'or looté sur un monstre sous le seuil parangon, dans la meme
    // proportion que la reduction de degats (equite de combat) - le
    // principe est le meme : neutraliser le bonus de stats parangon dans ce
    // gain precis, pour que le loot reste equivalent a un kill seuil vs
    // seuil.
    //
    // Portee volontairement limitee au loot de creature (pas les
    // recompenses d'or de quete, pas l'or de vente/reparation/courrier) -
    // coherent avec le fait que seul le combat contre du contenu sous-niveau
    // doit etre neutralise, pas la progression economique generale.
    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (!ParangonConfig::Enabled || !IsParangon(player) || !loot)
            return;

        // TODO: verifier sur ta version du core que GetLootGUID() +
        // ObjectAccessor::GetCreature() est bien le moyen correct de
        // retrouver la creature source du loot en cours depuis ce hook -
        // c'est l'approche standard mais pas verifiee ligne a ligne ici.
        Creature* creature = ObjectAccessor::GetCreature(*player, player->GetLootGUID());
        if (!creature)
            return; // pas un loot de creature (loot d'un autre joueur, objet au sol, etc.)

        if (creature->GetLevel() >= ParangonConfig::Threshold)
            return; // monstre deja au niveau seuil ou au-dessus : pas de correction

        float statFactor = GetParangonStatFactor(player);
        if (statFactor <= 0.0f)
            return;

        float correction = 1.0f + ((statFactor - 1.0f) * ParangonConfig::DamageEquityFactor);
        if (correction <= 0.0f)
            return;

        // TODO: verifier le nom exact du champ d'or sur Loot dans ta
        // version du core (souvent "gold", parfois "loot_money" ou via un
        // accesseur dedie plutot qu'un membre public direct).
        loot->gold = static_cast<uint32>(loot->gold / correction);
    }

    // NOTE: nom/signature a verifier sur ta version du core (meme
    // incertitude que pour OnGiveXP/OnLogin plus haut) - suppose ici
    // "Player*, uint32 p_time" par analogie avec les autres hooks de tick.
    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (!ParangonConfig::Enabled || !ParangonConfig::ForceAggroEnabled || !IsParangon(player))
            return;

        ObjectGuid guid = player->GetGUID();
        bool shouldCheck = false;

        // Protection par mutex : cette map est partagee entre TOUS les
        // joueurs (la classe de script est un singleton), et OnPlayerUpdate
        // peut etre appele depuis des threads differents si le coeur traite
        // plusieurs cartes en parallele. Un acces concurrent non protege a
        // une std::unordered_map corrompt sa structure interne - c'est tres
        // probablement la cause d'un crash STATUS_HEAP_CORRUPTION observe
        // sans cette protection.
        {
            std::lock_guard<std::mutex> lock(m_aggroCheckTimersMutex);
            uint32& timer = m_aggroCheckTimers[guid];

            if (timer <= p_time)
            {
                timer = ParangonConfig::ForceAggroCheckIntervalMs;
                shouldCheck = true;
            }
            else
            {
                timer -= p_time;
            }
        }

        if (shouldCheck)
            ForceAggroNearbyMobs(player);
    }

private:
    // Timer de scan de proximite par joueur (la classe script est un
    // singleton partage entre tous les joueurs, d'ou la necessite d'une
    // map indexee par GUID plutot qu'un simple membre) + mutex de
    // protection contre les acces concurrents (cf. commentaire ci-dessus).
    std::mutex m_aggroCheckTimersMutex;
    std::unordered_map<ObjectGuid, uint32> m_aggroCheckTimers;

    // Force l'engagement des monstres hostiles sous le seuil parangon situes
    // a portee fixe (Parangon.ForceAggroRangeYards) du joueur, independamment
    // de la portee d'aggro native (qui depend du vrai niveau du joueur, non
    // du palier parangon). Ne touche qu'aux creatures deja hostiles a la
    // faction du joueur (IsHostileTo) : un animal passif par template ne
    // devient pas agressif via ce mecanisme, seul le rayon de detection est
    // court-circuite pour les monstres deja hostiles.
    void ForceAggroNearbyMobs(Player* player)
    {
        std::list<Creature*> nearbyCreatures;

        // Pattern de recherche en grille confirme via Cell.h : VisitObjects
        // (et non VisitAllObjects, qui n'existe pas dans cette version).
        Acore::AnyUnitInObjectRangeCheck check(player, ParangonConfig::ForceAggroRangeYards);
        Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(player, nearbyCreatures, check);
        Cell::VisitObjects(player, searcher, ParangonConfig::ForceAggroRangeYards);

        for (Creature* creature : nearbyCreatures)
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat())
                continue;

            if (creature->GetLevel() >= ParangonConfig::Threshold)
                continue; // deja au niveau seuil ou au-dessus, l'aggro native suffit

            if (!creature->IsHostileTo(player))
                continue; // ne rend pas hostile un monstre passif par template

            if (CreatureAI* ai = creature->AI())
                ai->AttackStart(player);
        }
    }

private:
    // Applique (ou retire) le bonus de stats correspondant a un nombre de
    // paliers parangon donne, directement via le systeme UnitMods du
    // joueur (Unit::ApplyStatPctModifier, herite par Player) plutot que via
    // un spell/aura custom - evite d'avoir a definir une entree DBC/SQL pour
    // un simple pourcentage de stats.
    //
    // NOTE: ApplyStatPctModifier ne prend PAS de parametre bool "apply" - le
    // montant est signe : positif pour ajouter le pourcentage, negatif pour
    // le retirer.
    void ApplyParangonStatModifier(Player* player, uint32 parangonLevel, bool apply)
    {
        float pct = parangonLevel * ParangonConfig::StatsPerLevelPct;
        if (pct <= 0.0f)
            return;

        if (!apply)
            pct = -pct;

        static UnitMods const statMods[MAX_STATS] =
        {
            UNIT_MOD_STAT_STRENGTH,
            UNIT_MOD_STAT_AGILITY,
            UNIT_MOD_STAT_STAMINA,
            UNIT_MOD_STAT_INTELLECT,
            UNIT_MOD_STAT_SPIRIT
        };

        for (uint8 i = 0; i < MAX_STATS; ++i)
            player->ApplyStatPctModifier(statMods[i], TOTAL_PCT, pct);

        player->UpdateAllStats();
    }

    // "Pont" de niveau sous le seuil parangon (60-79 typiquement) : gere le
    // gain d'XP nous-memes, en contournant completement GiveXP()/"amount",
    // pour rester fonctionnel meme si un autre module (ex:
    // mod-individual-progression) bloque "amount" a 0 dans son propre
    // OnPlayerGiveXP. Utilise le cout XP NATIF (sObjectMgr, deja valide
    // jusqu'au seuil - pas besoin de notre propre formule ici) plutot que
    // GetParangonXPRequired, pour rester une progression WotLK normale.
    //
    // LIMITE CONNUE : si le module bloquant utilise aussi
    // PLAYERHOOK_ON_CAN_GIVE_LEVEL (ou equivalent) pour empecher
    // explicitement GiveLevel(), ce contournement ne suffira pas - a
    // verifier en jeu.
    void GrantBridgeXP(Player* player, Creature* victim)
    {
        uint32 gain = ComputeNativeKillXPGain(player, victim);

        LOG_INFO("module", "[mod-parangon] branche PONT {} (lvl {}) vs {} (lvl {}) -> gain={}",
            player->GetName(), player->GetLevel(), victim->GetName(), victim->GetLevel(), gain);

        if (gain == 0)
            return;

        uint32 currentXP = player->GetUInt32Value(PLAYER_XP);
        uint32 newXP = currentXP + gain;

        // TODO: verifier le nom exact de l'accesseur sur ta version du core
        // (sObjectMgr->GetXPForLevel() est le nom historique).
        uint32 required = sObjectMgr->GetXPForLevel(player->GetLevel());

        while (required > 0 && newXP >= required && player->GetLevel() < ParangonConfig::Threshold)
        {
            newXP -= required;
            player->GiveLevel(player->GetLevel() + 1);
            required = sObjectMgr->GetXPForLevel(player->GetLevel());
        }

        SyncNativeXPBar(player, newXP, required);

        LOG_INFO("module", "[mod-parangon] PONT {} : gain={} currentXP={} newXP={} required={}",
            player->GetName(), gain, currentXP, newXP, required);
    }

    // Ecrit directement PLAYER_XP/PLAYER_NEXT_LEVEL_XP - ce champ natif
    // sert a la fois d'affichage ET de compteur persistant (sauvegarde de
    // personnage standard, pas de stockage separe). L'ecrire directement
    // (sans passer par GiveXP()) ne declenche aucune logique de level up
    // native - seul GiveLevel() (appele explicitement dans GrantParangonXP)
    // fait progresser le niveau.
    //
    // TODO: verifier les noms de champs exacts sur ta version du core
    // (PLAYER_XP / PLAYER_NEXT_LEVEL_XP sont les noms historiques
    // Trinity/AzerothCore, mais peuvent avoir change de nom selon la
    // revision).
    void SyncNativeXPBar(Player* player, uint32 currentXP, uint32 requiredXP)
    {
        player->SetUInt32Value(PLAYER_XP, currentXP);
        player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, requiredXP);
    }

    // Accumule l'XP parangon (compteur = champ natif PLAYER_XP, pas de
    // stockage separe) et declenche autant de GiveLevel() que necessaire,
    // sans jamais laisser le coeur gerer lui-meme la boucle de level up
    // (player_xp_for_level n'est pas etendue au-dela du seuil).
    void GrantParangonXP(Player* player, uint32 gain)
    {
        uint32 currentXP = player->GetUInt32Value(PLAYER_XP);
        uint32 newXP = currentXP + gain;

        uint32 parangonLevel = GetParangonLevel(player);
        uint32 required = GetParangonXPRequired(parangonLevel);

        bool leveledUp = false;

        while (newXP >= required && player->GetLevel() < ParangonConfig::MaxLevel)
        {
            newXP -= required;
            player->GiveLevel(player->GetLevel() + 1); // peut re-ecrire PLAYER_XP/PLAYER_NEXT_LEVEL_XP avec des valeurs natives incorrectes - corrige juste apres par SyncNativeXPBar
            leveledUp = true;

            parangonLevel = GetParangonLevel(player);
            required = GetParangonXPRequired(parangonLevel);
        }

        // Ecrase PLAYER_XP/PLAYER_NEXT_LEVEL_XP avec nos propres valeurs -
        // la barre d'XP native affiche donc la vraie progression parangon,
        // sans jamais passer par la boucle de level up native.
        SyncNativeXPBar(player, newXP, required);

        // Log serveur (console/fichier, PAS le chat en jeu) pour diagnostic.
        // TODO: verifier le nom du filtre de log sur ta version du core -
        // "module" est un filtre generique courant dans les modules AC,
        // mais peut necessiter d'etre ajoute a logging.conf pour etre
        // visible en console selon la config LogLevel par filtre.
        LOG_INFO("module", "[mod-parangon] {} : gain={} currentXP={} newXP={} required={} leveledUp={}",
            player->GetName(), gain, currentXP, newXP, required, leveledUp);
    }
};

// ---------------------------------------------------------------------
// UnitScript : equite de combat contre le contenu sous le seuil
// ---------------------------------------------------------------------

class Parangon_UnitScript : public UnitScript
{
public:
    // NOTE: UnitScript n'a pas ete signale en erreur dans ton build - le
    // constructeur a un seul argument (sans liste de hooks) semble donc
    // encore accepte pour ce type de script sur ta version du core. Si un
    // futur recompile signale une erreur similaire (C3668) sur
    // ModifyMeleeDamage/ModifySpellDamageTaken, applique le meme correctif
    // que pour PlayerScript : liste de hooks dans le constructeur + noms de
    // methode a verifier.
    Parangon_UnitScript() : UnitScript("Parangon_UnitScript") {}

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        ApplyEquityFactorUnsigned(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        uint32 unsignedDamage = static_cast<uint32>(damage);
        ApplyEquityFactorUnsigned(target, attacker, unsignedDamage);
        damage = static_cast<int32>(unsignedDamage);
    }

private:
    // Reduit les degats infliges PAR le joueur (ou son familier) AU
    // monstre, et augmente les degats infliges PAR le monstre AU
    // joueur/familier, avec le MEME facteur (GetCombatEquityFactor, base
    // sur l'ecart de niveau reel avec le seuil, pas sur le palier parangon
    // du joueur) - c'est ce qui neutralise l'ecart de puissance NATIF
    // (gear, stats de base), pas seulement le petit bonus de stats
    // parangon. Ne modifie jamais les stats du monstre - seul le nombre de
    // degats deja calcule par le moteur est ajuste, apres resolution du
    // coup. Les "ratés" lies a l'ecart de niveau ne sont volontairement pas
    // traites ici.
    void ApplyEquityFactorUnsigned(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!ParangonConfig::Enabled || !target || !attacker)
            return;

        if (GetControllingParangonPlayer(attacker))
        {
            // Le joueur (ou son familier) est l'attaquant.
            Creature* mob = target->ToCreature();
            float factor = GetCombatEquityFactor(mob);
            if (factor <= 1.0f)
                return;

            damage = static_cast<uint32>(damage / factor);
        }
        else if (GetControllingParangonPlayer(target))
        {
            // Le joueur (ou son familier) est la cible.
            Creature* mob = attacker->ToCreature();
            float factor = GetCombatEquityFactor(mob);
            if (factor <= 1.0f)
                return;

            damage = static_cast<uint32>(damage * factor);
        }
    }
};

// ---------------------------------------------------------------------
// Enregistrement des scripts
// ---------------------------------------------------------------------

// Le loader de modules genere automatiquement un appel a une fonction
// nommee d'apres le dossier du module : mod-parangon -> Addmod_parangonScripts.
// Le nom doit correspondre exactement (verifie par l'erreur de link
// LNK2019 rencontree), pas de "AddSC_" prefixe ici.
void Addmod_parangonScripts()
{
    new Parangon_WorldScript();
    new Parangon_PlayerScript();
    new Parangon_UnitScript();
}
