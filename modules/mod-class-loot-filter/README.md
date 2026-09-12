# mod-class-loot-filter

Module AzerothCore (WoW 3.3.5a) qui évite de looter des équipements **BoE**
(Bind on Equip) inutilisables par la classe du joueur — typiquement une pièce
en plaques qui tombe pour un mage — sur les créatures et les coffres.

## Fonctionnement

À chaque loot d'objet (`PlayerScript::OnLootItem`) :

1. Le module ignore tout ce qui n'est pas une armure/arme BoE (configurable).
2. Il détermine si l'objet est utilisable par la classe du joueur :
   - restriction `AllowableClass` de la table `item_template` (reliques, etc.) ;
   - proficience d'armure (tissu/cuir/mailles/plaques, boucliers) — activée
     par défaut ;
   - proficience d'arme — table approximative, **désactivée par défaut**
     (`ClassLootFilter.FilterWeapons = 0`), à valider avant activation.
3. Si l'objet convient : rien ne change.
4. Si l'objet ne convient pas :
   - il est retiré de l'inventaire du joueur ;
   - **fallback 1** : le module cherche dans le même butin (même créature/
     coffre) un autre objet encore disponible et utilisable par la classe, et
     le donne à la place ;
   - **fallback 2** (si rien d'utile dans ce butin) : compensation en or,
     basée sur le prix de vente vendeur de l'objet retiré
     (`ClassLootFilter.GoldCompensationPct`).

## Installation

1. Copier ce dossier dans `modules/mod-class-loot-filter` de votre arbre
   AzerothCore.
2. Recompiler (les modules modernes n'ont pas besoin de `CMakeLists.txt`,
   le dossier `src`/racine est auto-détecté).
3. Copier `mod-class-loot-filter.conf.dist` vers `mod-class-loot-filter.conf`
   dans votre dossier de config si vous voulez modifier les valeurs par
   défaut.

## Points à vérifier avant compilation

Ce squelette a été écrit à partir de la documentation Doxygen d'AzerothCore ;
comme pour vos autres modules, mieux vaut confirmer par un grep sur votre
arbre local avant de lancer le build complet :

- `PlayerScript::OnLootItem(Player*, Item*, uint32, ObjectGuid)` — signature
  du hook (`PlayerScript.h`).
- `creature->loot` / `go->loot` — nom du membre `Loot` sur `Creature` et
  `GameObject`.
- `Player::StoreNewItemInBestSlots(uint32 itemId, uint32 count)` — utilisé
  pour donner l'objet de remplacement.
- `LootItem::itemid`, `LootItem::is_looted`, `LootItem::AllowedForPlayer(...)`
  (`LootMgr.h`).
- `ObjectGuid::IsCreatureOrVehicle()` / `IsGameObject()`.
- Nom de la fonction de chargement `Addmod_class_loot_filterScripts()` :
  à adapter selon la convention de votre chargeur de modules (comme cela
  avait été fait pour mod-parangon → `Addmod_parangonScripts`).

## Limites connues

- La table de proficience d'armes est une approximation des règles
  classiques WotLK ; elle reste désactivée par défaut pour cette raison.
- Le remplacement "objet alternatif dans le même butin" (fallback 1) donne
  l'objet via `StoreNewItemInBestSlots`, donc sans reproduire d'éventuels
  suffixes aléatoires du roll d'origine — c'est une simplification
  volontaire.
- Non testé spécifiquement avec le butin de groupe en mode "Maître du
  butin" (Master Loot) ; le comportement par défaut (loot individuel /
  besoin-cupidité) est le cas visé en priorité.
