# mod-parangon

Module AzerothCore (WoW 3.3.5a) — au-dela d'un seuil parametrable (60 par
defaut — voir aussi la note de compatibilite avec `individual-progression`
plus bas), le level up classique est remplace par une progression de stats
brutes (+X%/palier), tout en neutralisant l'ecart de puissance contre le
contenu sous-niveau. Aucune table SQL, aucun DBC custom : XP, leveling et
bonus de stats sont geres entierement en C++ (voir "Comment ca marche").

## Installation

1. Copier ce dossier dans `modules/mod-parangon` (pas de `CMakeLists.txt`
   necessaire, le build moderne detecte `src/` automatiquement).
2. Relever `MAX_LEVEL` (et `STRONG_MAX_LEVEL` si present) dans
   `SharedDefines.h`/`Common.h` et recompiler — c'est le seul patch core
   requis. `player_classlevelstats` n'a pas besoin d'etre etendue (le core
   duplique deja les stats du dernier niveau connu).
3. Copier `conf/mod_parangon.conf.dist` vers `env/dist/etc/modules/`.
4. Recompiler.

## Compatibilite avec mod-individual-progression

Ce serveur utilise aussi `individual-progression`, qui bloque `amount` a 0
dans son propre `OnPlayerGiveXP` quand le joueur atteint 60 (ou 70) sans
avoir termine le raid correspondant. `Parangon.Threshold` est a 60 par
defaut, ce qui entre directement en conflit avec ce plafonnage - c'est
pour ca que `Parangon.BypassProgressionXPBlock` (actif par defaut) existe :
il fait gerer le gain d'XP de kill sous le seuil directement par
`mod-parangon`, via `OnPlayerCreatureKill` (qui n'utilise pas `amount` et
n'est donc pas affecte par ce que l'autre module y fait) - avec le cout XP
natif (`sObjectMgr->GetXPForLevel`), pour une progression normale jusqu'au
seuil malgre le blocage externe.

**Limite connue, non testee** : si `individual-progression` bloque aussi
via un hook du type "CanGiveLevel" (empechant explicitement le level up,
pas seulement l'XP), ce contournement ne suffira pas a lui seul - a
verifier en jeu. Les recompenses de **quete** (`OnPlayerGiveXP`) restent
elles soumises au blocage externe, puisqu'on ne peut pas recuperer la
valeur d'origine une fois qu'elle a ete mise a 0 par l'autre module avant
que notre hook s'execute.

## Configuration

| Cle                            | Defaut | Description                                                  |
|----------------------------------|--------|------------------------------------------------------------------|
| `Parangon.Enable`                | 1      | Active/desactive le module                                       |
| `Parangon.Threshold`             | 60     | Niveau a partir duquel le mode parangon s'active                  |
| `Parangon.MaxLevel`              | 250    | Niveau max (<= MAX_LEVEL compile dans le core)                    |
| `Parangon.StatsPerLevelPct`      | 1.0    | % de bonus de stats par palier parangon                          |
| `Parangon.DamageEquityFactor`    | 1.0    | Ajustement fin de la neutralisation de degats/or                  |
| `Parangon.XPBaseRequirement`     | 50000  | XP requise pour le 1er palier parangon                            |
| `Parangon.XPGrowthPct`           | 5.0    | Croissance en % de l'XP requise par palier                       |
| `Parangon.QuestXPCapPct`         | 5.0    | Plafond de l'XP de quete (% de l'XP requise pour le palier)       |
| `Parangon.AnnounceXPGain`        | 1      | Message chat a chaque gain d'XP parangon                          |
| `Parangon.ForceAggroEnabled`     | 1      | Force l'engagement des monstres hostiles sous le seuil a proximite |
| `Parangon.ForceAggroRangeYards`  | 20.0   | Portee du scan de proximite pour l'aggro forcee                   |
| `Parangon.MobDamageBoostPctPerLevel` | 3.0 | % de degats en plus (compose) par niveau d'ecart sous le seuil, degats monstre -> joueur |
| `Parangon.MobDamageBoostMaxFactor`   | 5.0 | Plafond du multiplicateur ci-dessus                                |
| `Parangon.BypassProgressionXPBlock`  | 1   | Contourne un blocage externe de l'XP sous le seuil (ex: individual-progression) pour les kills |

## Comment ca marche

- **Stats** : `Parangon_PlayerScript` applique/retire un modificateur
  pourcentage direct sur les `UnitMods` du joueur (`ApplyStatPctModifier`)
  a chaque connexion/niveau — pas de spell/aura custom.
- **XP/leveling** : une fois le seuil atteint, le module met `amount = 0`
  dans `OnPlayerGiveXP` pour empecher la boucle de level up native (basee
  sur `player_xp_for_level`, non etendue au-dela du seuil), calcule
  lui-meme l'XP requise par palier (formule geometrique C++,
  `GetParangonXPRequired`) et declenche `player->GiveLevel()` a la main.
  Le champ natif `PLAYER_XP` sert directement de compteur (persiste via la
  sauvegarde de personnage standard, pas de stockage separe) ; `PLAYER_XP`
  et `PLAYER_NEXT_LEVEL_XP` sont resynchronises a chaque gain
  (`SyncNativeXPBar`) pour que la barre d'XP native reste coherente, sans
  jamais laisser le coeur gerer le level up lui-meme.
- **Equite de combat et or** : voir section dediee ci-dessous.

## Equite de combat : exemple concret (joueur parangon 64 vs mob niveau 5)

Seuil = 60, `StatsPerLevelPct` = 1.0, `DamageEquityFactor` = 1.0,
`MobDamageBoostPctPerLevel` = 3.0.

**Sens joueur -> monstre** (reduction, neutralise le bonus parangon) :
1. Palier parangon = 64 - 60 = 4 -> `statFactor = 1.04`.
2. Mob niveau 5 < seuil -> `correction = 1.04`.
3. Degats bruts calcules par le moteur (bonus parangon deja inclus) : 500
   -> applique : `500 / 1.04 ≈ 480`.

**Sens monstre -> joueur** (augmentation, compense la faiblesse native du
monstre - SANS modifier ses stats reelles) :
1. Ecart de niveau avec le seuil : `60 - 5 = 55`.
2. `boost = 1.03^55 ≈ 5.1`, plafonne a `MobDamageBoostMaxFactor` (5.0 par
   defaut).
3. Si le monstre inflige 10 degats bruts au joueur, le hook applique
   `10 * 5.0 = 50`.

Les "ratés" lies a l'ecart de niveau (chance de toucher/esquiver/parer) ne
sont volontairement pas traites : ces hooks n'interviennent qu'une fois
qu'un coup a deja touche, sur le nombre de degats final uniquement.

Cette equite couvre aussi les **familiers** du joueur (via leur
proprietaire, `GetOwner()`) : un familier qui frappe ou qui est frappe par
un monstre sous le seuil est corrige de la meme facon que le joueur
lui-meme.

Le meme principe (`statFactor`/`DamageEquityFactor`) est reutilise pour
l'or : `OnPlayerBeforeLootMoney` reduit l'or looté sur un monstre sous le
seuil, en recuperant la creature source via `player->GetLootGUID()` +
`ObjectAccessor::GetCreature` (a verifier sur ta version du core).

## Aggro / threat : PAS gere

Aucun recalcul specifique n'a ete fait pour la generation de menace
(threat). Le raisonnement : le threat est genere proportionnellement aux
degats infliges (coefficients de menace des sorts/coups), donc en
normalisant deja les degats (`ModifyMeleeDamage`/`ModifySpellDamageTaken`),
la menace generee suit naturellement la meme reduction — sans intervention
separee. **Non teste en jeu.**

En revanche, un point distinct et reellement non traite : le **rayon de
detection** des monstres (aggro range, pas le threat) scale generalement
avec l'ecart de niveau dans le coeur — un personnage tres au-dessus du
niveau d'un monstre peut donc se faire agresser de plus loin que prevu.
Ce module ne touche pas ce mecanisme.

## Audit des sources d'XP (`PlayerXPSource`)

| Source                | Statut | Detail |
|------------------------|--------|--------|
| `XPSOURCE_KILL`         | ✅ | `OnPlayerCreatureKill` (pas `OnPlayerGiveXP`, qui ne se declenche pas pour les kills "gris") -> `BaseGain` niveau victime force au niveau joueur + bonus elite/donjon + `Rate.XP.Kill` + bonus de groupe |
| `XPSOURCE_QUEST` / `_DF` | ✅ | Montant natif plafonne a `QuestXPCapPct` % de l'XP du palier courant |
| `XPSOURCE_EXPLORE`      | ⚠️ | Non geree, gain = 0 |
| `XPSOURCE_BATTLEGROUND` | ⚠️ | Non geree, gain = 0 |
| Bonus de repos          | ⚠️ | Non reapplique pour les kills (le multiplicateur natif est perdu) |
| Money-instead-of-XP hook | ⚠️ | Non verifie, risque d'interference proche de `Parangon.MaxLevel` |

## Prochaines etapes

1. Verifier les TODO marques dans `src/mod-parangon.cpp` (signatures API :
   `ApplyStatPctModifier`, `GetPlayerSetting`/`UpdatePlayerSetting`,
   `GetContentLevelsForMapAndZone`, acces creature depuis `Loot`).
2. Tester en jeu l'equite de combat (parangon 0 vs 20) et l'or.
3. Traiter `XPSOURCE_EXPLORE`/`XPSOURCE_BATTLEGROUND`/bonus de repos si
   pertinent pour le serveur.
4. Decider si le rayon d'aggro merite d'etre corrige aussi.