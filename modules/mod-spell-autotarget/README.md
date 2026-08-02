# mod-spell-autotarget

Module C++ pour [AzerothCore](https://www.azerothcore.org/) (WoW 3.3.5).

Quand un joueur (ou un gardien : pet, familier de chasseur, totem...) lance un
sort dont l'effet nécessite normalement de cliquer au sol pour choisir la zone
d'impact (ex: **Blizzard**), ce module repositionne automatiquement cette
destination sur la position de la **cible actuelle** du lanceur (cible
attaquée, ou cible sélectionnée pour un joueur), quel que soit l'endroit
réellement cliqué.

## ⚠️ Ce que ce module fait — et ne fait pas

- Il **ne dispense pas** de cliquer une fois au sol : cette étape (le réticule
  de visée) est gérée entièrement par le client WoW et aucun module serveur
  ne peut la supprimer. C'est une contrainte du protocole client/serveur, pas
  une limite de ce module.
- Il fait en sorte que **l'endroit précis du clic n'a plus d'importance** :
  peu importe où vous cliquez, l'effet atterrira sur votre cible actuelle.
- Il fonctionne pour tous les sorts qui utilisent un effet de type
  "destination au sol" (`TARGET_DEST_DEST`), détectés automatiquement au
  chargement du sort — pas besoin de maintenir une liste d'IDs à la main
  (une liste d'exclusion reste disponible si besoin).
- Il s'applique aussi bien aux sorts lancés par les joueurs qu'aux sorts
  lancés par leurs gardiens (pets, totems...), puisque le hook utilisé
  s'applique à n'importe quel `Unit`.

## Commande joueur : `.groundcast`

Le module ajoute aussi une commande accessible à **tous les joueurs** (pas
besoin de droits GM) :

```
.groundcast Blizzard
.groundcast 42208
```

Une commande est traitée entièrement côté serveur : il n'y a donc **aucun
réticule à cliquer**, contrairement au clic sur l'icône du sort. Le sort est
lancé exactement comme un cast normal :

- le joueur doit connaître le sort (`HasSpell`) ;
- mana, temps de recharge, portée et ligne de vue sont vérifiés normalement ;
- si le sort cible une unité, il est lancé sur la cible actuelle ;
- si le sort nécessite une destination au sol (comme Blizzard), la
  destination est calculée automatiquement sur la position de la cible
  actuelle (ou sous les pieds du joueur si aucune cible n'est sélectionnée).

Ce n'est pas un raccourci "gratuit" façon commande GM : ça se comporte comme
un vrai cast, juste sans avoir besoin de viser au sol.

Peut être désactivé indépendamment du reste via `SpellAutoTarget.EnableCommand`.

## Complément gratuit, 100% côté client

Si tu veux carrément supprimer le besoin de cliquer, sans mod ni add-on,
utilise une macro standard (fonctionne nativement en 3.3.5) :

```
#showtooltip
/cast [target=target,exists] NomDuSort
```

Cette macro lance directement le sort sur la position de la cible
sélectionnée, sans jamais afficher le réticule. Ce module et la macro sont
complémentaires : la macro règle le clic, le module règle la précision (utile
si un joueur clique sans macro, ou si un gardien lance le sort seul).

## Installation

1. Cloner ce module dans le dossier `modules/` de ton code source AzerothCore :
   ```
   cd azerothcore-wotlk/modules
   git clone <url-de-ce-repo> mod-spell-autotarget
   ```
2. Relancer CMake puis recompiler le serveur (`worldserver`).
3. Au premier démarrage, copier `mod_spell_autotarget.conf.dist` en
   `mod_spell_autotarget.conf` dans le dossier de config du serveur, et
   ajuster si besoin.

## Configuration

Voir [`conf/mod_spell_autotarget.conf.dist`](conf/mod_spell_autotarget.conf.dist) :

- `SpellAutoTarget.Enable` — active/désactive le module.
- `SpellAutoTarget.IncludeGuardians` — inclut ou non les pets/totems/gardiens.
- `SpellAutoTarget.ExcludedSpells` — liste d'IDs de sorts à exclure (virgules).

## Comment ça marche techniquement

Le module s'accroche au hook `AllSpellScript::OnSpellPrepare`, appelé juste
après l'initialisation des cibles explicites du sort (donc après lecture de la
position cliquée par le client) mais **avant** la résolution finale des
cibles touchées par l'effet (`SelectSpellTargets`). À ce moment, si le sort a
un effet `TARGET_DEST_DEST` (position au sol), le module remplace la
destination par la position de la cible actuelle du lanceur — sous réserve
qu'elle soit à portée du sort.

## Limitations connues

- Un sort sans cible sélectionnée/attaquée gardera son comportement normal
  (position cliquée, ou position du lanceur si aucune position n'a été
  fournie par le client).
- Si la cible est hors de portée du sort, le module n'intervient pas (pour
  éviter un échec de lancement inattendu) : le comportement normal s'applique.
