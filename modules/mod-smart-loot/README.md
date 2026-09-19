# mod-class-loot-filter

Module AzerothCore (WoW 3.3.5a) avec **deux mécanismes distincts** pour
éviter le loot "gâché" par rapport à la classe des joueurs humains :

1. **BoE** (Bind on Equip) sur créatures et coffres, tous types de mobs.
2. **Smart loot BoP** (Bind on Pickup) sur les boss de donjon/raid
   uniquement.

## 1) BoE — créatures et coffres

Au moment du loot (`PlayerScript::OnLootItem`) :

1. Le module ignore tout ce qui n'est pas une armure/arme BoE (configurable).
2. Il détermine si l'objet est utilisable/préféré par la classe du joueur :
   - restriction `AllowableClass` de la table `item_template` (reliques, etc.) ;
   - **armure, mode strict par défaut** (`ClassLootFilter.StrictArmorPreference = 1`) :
     chaque classe n'accepte qu'un seul type d'armure "préféré", avec repli
     automatique si le skill correspondant n'est pas encore acquis :

     | Classe | Préféré | Repli si skill manquant |
     |---|---|---|
     | Mage / Prêtre / Démoniste | Tissu | — |
     | Voleur / Druide | Cuir | — |
     | Chasseur / Chaman | Mailles | Cuir |
     | Guerrier / Paladin / Chevalier de la mort | Plaques | Mailles |

     Boucliers/reliques (libram, idole, totem, sceau) restent gérés à part,
     selon la classe autorisée à les porter. Les capes/bijoux (sous-classe
     "divers") ne sont jamais filtrés.
     Repasser à `ClassLootFilter.StrictArmorPreference = 0` retrouve l'ancien
     comportement permissif (tout ce que la classe peut légalement porter).
   - proficience d'arme — table approximative, **désactivée par défaut**
     (`ClassLootFilter.FilterWeapons = 0`), à valider avant activation.
3. Si l'objet convient : rien ne change.
4. Si l'objet ne convient pas :
   - il est retiré de l'inventaire du joueur ;
   - **fallback 1** : le module cherche dans le même butin (même créature/
     coffre) un autre objet encore disponible et utilisable par la classe, et
     le donne à la place ;
   - **fallback 2** : si nécessaire, il cherche un objet compatible de même
     qualité dans la table de loot possible du coffre et le donne à la place ;
   - **fallback 3** (si aucun objet compatible n'existe) : l'objet d'origine
     est rendu au joueur.

Deux options dédiées au contexte boss pour ce mécanisme BoE :

- `ClassLootFilter.OnlyBosses` : si activé, le mécanisme BoE ne s'applique
  plus qu'aux créatures marquées "Dungeon Boss" ; trashs et coffres ne sont
  plus touchés. Désactivé par défaut.
## 2) Smart loot BoP — boss de donjon/raid uniquement

Mécanisme totalement différent, indépendant du BoE, qui agit **avant** le
tirage besoin/cupidité, au moment où le core décide si une pièce de la
table de butin du boss doit même apparaître
(`GlobalScript::OnItemRoll`).

Pour chaque pièce d'armure/arme **soulbound (BoP)** d'un boss
(`Creature::IsDungeonBoss()`), la chance de drop est :

- multipliée par `ClassLootFilter.SmartLootBoostFactor` (défaut **2.0**) si
  la pièce est utilisable par **au moins un joueur humain** actuellement
  dans le groupe (mêmes règles de préférence de classe que le BoE
  ci-dessus : `FilterArmor`, `StrictArmorPreference`, `FilterWeapons`) ;
- multipliée par `ClassLootFilter.SmartLootPenaltyFactor` (défaut **0.3**,
  volontairement non nul pour ne pas vider les tables de butin) sinon.

Les **playerbots sont exclus** du calcul (`ClassLootFilter.ExcludeBots`) :
seules les classes des joueurs humains du groupe comptent pour décider quoi
favoriser. En solo, seul le joueur lui-même compte.

Activation : `ClassLootFilter.SmartLootEnable` (activé par défaut).

**Limite importante à connaître :** ce mécanisme influence uniquement la
*probabilité* qu'une pièce apparaisse dans le butin du boss (comme le
"smart loot" côté Blizzard). Il ne peut pas forcer un joueur précis à
gagner le tirage besoin/cupidité ensuite — ce tirage reste entièrement géré
par le système de groupe natif du core. Concrètement : plus de plaques
utiles au guerrier apparaîtront statistiquement sur un boss si le groupe
contient un guerrier humain, mais rien n'empêche qu'un autre joueur humain
du groupe la gagne au tirage.

## Exemple (votre configuration)

Groupe : vous (chasseur), un ami (mage), 3 bots (paladins).

- **BoE** looté par vous : filtré selon votre classe/skill (mailles si
  skill acquis, sinon cuir). BoE de votre ami : tissu uniquement. BoE des
  3 bots : jamais touché.
- **BoP de boss** : la chance de drop des pièces plaques (utiles à un
  paladin, mais les 3 paladins sont des bots exclus du calcul) n'est PAS
  boostée par leur présence — seules vos préférences (chasseur → mailles/
  cuir) et celles de votre ami (mage → tissu) comptent pour la pondération.

## Installation

1. Copier ce dossier dans `modules/mod-class-loot-filter` de votre arbre
   AzerothCore, en conservant la structure :
   ```
   mod-class-loot-filter/
   ├── conf/
   │   └── mod-class-loot-filter.conf.dist
   ├── mod-class-loot-filter.cpp
   └── README.md
   ```
   Le fichier de config DOIT être dans un sous-dossier `conf/` : c'est ce
   dossier que le core scanne pour copier automatiquement les `.conf.dist`
   vers `env/dist/etc/modules/` lors du `cmake`/build (un `.conf.dist` posé
   à la racine du module n'est pas repris).
2. Relancer `cmake` puis recompiler (les modules modernes n'ont pas besoin
   de `CMakeLists.txt`, le dossier racine est auto-détecté pour le code
   source).
3. Après le build, `mod-class-loot-filter.conf.dist` doit apparaître dans
   `env/dist/etc/modules/`. Copier/renommer ce fichier en
   `mod-class-loot-filter.conf` dans votre dossier de config final si vous
   voulez modifier les valeurs par défaut.

## Points à vérifier avant compilation

Ce squelette a été écrit à partir de la documentation Doxygen d'AzerothCore ;
comme pour vos autres modules, mieux vaut confirmer par un grep sur votre
arbre local avant de lancer le build complet :

- `PlayerScript::OnLootItem(Player*, Item*, uint32, ObjectGuid)` — signature
  du hook (`PlayerScript.h`).
- `GlobalScript::OnItemRoll(Player const*, LootStoreItem const*, float&
  chance, Loot&, LootStore const&)` — signature du hook (`ScriptMgr.h`).
- `creature->loot` / `go->loot` — nom du membre `Loot` sur `Creature` et
  `GameObject`.
- `Loot::sourceWorldObjectGUID` (`LootMgr.h`) — utilisé pour retrouver la
  créature depuis le hook `OnItemRoll`.
- `Player::StoreNewItemInBestSlots(uint32 itemId, uint32 count)` — utilisé
  pour donner l'objet de remplacement.
- `LootItem::itemid`, `LootItem::is_looted`, `LootItem::AllowedForPlayer(...)`
  (`LootMgr.h`).
- `ObjectGuid::IsCreatureOrVehicle()` / `IsGameObject()`.
- `Group::GetFirstMember()` / `GroupReference::next()` / `GetSource()`
  (`Group.h`) — itération des membres du groupe.
- `Creature::IsDungeonBoss() const` (`Creature.h`) — détection des boss
  de donjon/raid.
- Nom de la fonction de chargement `Addmod_class_loot_filterScripts()` :
  à adapter selon la convention de votre chargeur de modules (comme cela
  avait été fait pour mod-parangon → `Addmod_parangonScripts`).
- `WorldSession::IsBot()`, utilisé sous `#ifdef MOD_PLAYERBOTS` — compile
  à `false` si mod-playerbots n'est pas présent, sans dépendance dure sur
  un header externe.

## Limites connues

- La table de proficience d'armes est une approximation des règles
  classiques WotLK ; elle reste désactivée par défaut pour cette raison.
- Le remplacement "objet alternatif dans le même butin" (fallback BoE)
  donne l'objet via `StoreNewItemInBestSlots`, donc sans reproduire
  d'éventuels suffixes aléatoires du roll d'origine — simplification
  volontaire.
- Le smart loot BoP ne fait que pondérer une chance de drop ; il ne
  garantit jamais qu'un joueur précis gagne le tirage besoin/cupidité
  ensuite (voir la limite détaillée plus haut).
- Non testé spécifiquement avec le loot de groupe en mode "Maître du
  butin" (Master Loot) ; le comportement par défaut (besoin/cupidité) est
  le cas visé en priorité.