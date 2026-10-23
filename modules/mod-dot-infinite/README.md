# mod-dot-infinite

Module AzerothCore (WoW 3.3.5a) : les DoT lancés par un joueur durent indéfiniment sur leur cible. Ils ne s'arrêtent qu'en cas de dispel, de mort de la cible ou de reset/evade du monstre.

## Installation

1. Copier le dossier `mod-dot-infinite` dans `azerothcore/modules/`.
2. Relancer CMake puis recompiler le worldserver.
3. Copier `conf/mod_dot_infinite.conf.dist` en `mod_dot_infinite.conf` dans le dossier `configs/modules/` (si besoin) et l'ajuster.

## Fonctionnement

Le module s'abonne au hook `UnitScript::OnAuraApply`. Pour chaque aura appliquée, il vérifie que :

- le lanceur est un joueur (ou son pet si `DotInfinite.IncludePets = 1`) ;
- l'aura est négative, non canalisée et comporte un effet de dégâts périodiques (`PERIODIC_DAMAGE`, `PERIODIC_DAMAGE_PERCENT`, `PERIODIC_LEECH`) ;
- la cible n'est pas un joueur (sauf `DotInfinite.AffectPlayers = 1`) ;
- le sort n'est pas dans `DotInfinite.ExcludedSpells`.

Si tout est bon, la durée maximale de l'aura passe à -1 (permanente). Un rafraîchissement du DoT garde la durée à -1.

## Options

| Option | Défaut | Rôle |
|---|---|---|
| `DotInfinite.Enable` | 1 | Active le module |
| `DotInfinite.AffectPlayers` | 0 | Étend l'effet aux cibles joueurs |
| `DotInfinite.IncludePets` | 0 | Inclut les DoT des pets / gardiens |
| `DotInfinite.ExcludedSpells` | vide | Liste d'ID de sorts exclus |

## Remarques

- Les sorts canalisés (Drain de vie, Fouet mental...) sont exclus.
- L'aura s'affiche sans timer côté client.
- Si le lanceur se déconnecte, l'aura reste mais n'inflige plus de dégâts.
- Un DoT permanent maintient la cible en combat tant qu'il est actif.
- Vérifier que le hook `OnAuraApply` existe dans ton `UnitScript.h` ; sinon, il faut l'ajouter au core.
