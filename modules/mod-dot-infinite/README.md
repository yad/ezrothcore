# mod-dot-infinite

Module AzerothCore (WoW 3.3.5a) qui propage les DoT lancés par les joueurs.

- **Propagation** : deux familles de DoT, deux comportements.

## Installation

1. Copier le dossier `mod-dot-infinite` dans `azerothcore/modules/`.
2. Relancer CMake puis recompiler le worldserver.
3. Copier `conf/mod_dot_infinite.conf.dist` en `mod_dot_infinite.conf` dans `configs/modules/` et l'ajuster.

## Les deux familles de DoT

| | **DoT à distance** | **DoT de mêlée** |
|---|---|---|
| **Sorts** | tout sauf les deux types de droite : magie, malédictions, maladies, Moonfire, Insect Swarm, Serpent Sting... | saignements de toutes les classes (Rend, Rupture, Garrote, Deep Wounds, Rake, Rip, Lacerate...) et poisons du voleur (Deadly Poison) |
| **Se propage seul ?** | **Oui**, à toute cible en combat contre le joueur ou son groupe | **Non** |
| **Quand ?** | à la pose, à chaque tick, et quand un ennemi rejoint le combat | à chaque coup du joueur |
| **Où ?** | créatures engagées à moins de `Ranged.Range` yards d'une créature qui porte le DoT (60) | créatures engagées au corps à corps du joueur (portée de mêlée + `Melee.Range` yards) |
| **Si la cible l'a déjà** | ignorée (son timer n'est pas touché) | ignorée (son timer n'est pas touché) |

La famille est déterminée par le sort lui-même, pas par la classe du lanceur : un druide a ses saignements en mêlée et Moonfire à distance. L'école de magie n'est pas utilisée (les saignements sont Physiques, mais les poisons sont Nature, comme Moonfire).

### DoT à distance
Dès qu'un de ces DoT est sur un ennemi, il se répand à tous les ennemis engagés contre le joueur, son pet ou son groupe :

- à la pose ou au rafraîchissement du DoT ;
- à **chaque tick** : toute créature engagée qui ne l'a pas encore le reçoit ;
- à l'arrivée d'un **nouvel ennemi** dans le combat.

Les créatures de faction neutre sont également concernées dès qu'elles sont effectivement engagées en combat PvE avec un allié ; leur faction seule ne les exclut pas.

La portée est évaluée entre la créature qui porte déjà le DoT et chaque créature engagée visée. Elle ne dépend pas de la position du joueur ou de son pet.

### DoT de mêlée
Pas de propagation à la pose ni par tick. À chaque coup porté par le joueur (attaque normale réussie, ou capacité de mêlée qui touche), le module :

1. cherche les ennemis engagés contre le joueur, son pet ou son groupe ;
2. lit les DoT de mêlée du joueur présents sur l'un d'eux ;
3. les applique, avec le même nombre de charges, aux ennemis engagés dans la portée de mêlée du joueur à cet instant.

Une créature à proximité qui n'est pas en combat n'est jamais ciblée (pas d'aggro involontaire). Une évaluation est faite au plus tous les `DotInfinite.Melee.Interval` ms par joueur.

### Dans les deux cas
Les DoT à propager sont toujours lus sur les auras réellement présentes sur les ennemis. Un DoT dispel, expiré ou dont la cible est morte n'est donc plus propagé. Les copies sont lancées par le joueur, avec ses stats actuelles. La durée naturelle des sorts n'est pas modifiée ; une aura déjà présente n'est ni rafraîchie ni prolongée par la propagation.

## Quels DoT sont pris en compte ?

Un DoT est lancé par un joueur, négatif, non canalisé, et comporte un effet de dégâts périodiques (`PERIODIC_DAMAGE`, `PERIODIC_DAMAGE_PERCENT`, `PERIODIC_LEECH`). Sont exclus les sorts canalisés, les sorts à cible unique et les zones au sol (Consécration, Death and Decay, pièges...), dont la durée est celle de la zone et reste inchangée. Les DoT sur les joueurs gardent également leur durée naturelle et ne sont pas propagés.

## Options

| Option | Défaut | Rôle |
|---|---|---|
| `DotInfinite.Enable` | 1 | Active le module |
| `DotInfinite.Ranged.Enable` | 1 | Propagation automatique des DoT à distance |
| `DotInfinite.Ranged.Range` | 60 | Distance max entre créatures engagées pour propager un DoT (0 = illimitée) |
| `DotInfinite.Melee.Enable` | 1 | Application des DoT de mêlée au pack à chaque coup |
| `DotInfinite.Melee.Range` | 2 | Yards ajoutés à la portée de mêlée du joueur au moment du coup (0 = stricte) |
| `DotInfinite.Melee.Interval` | 250 | Délai min. entre deux évaluations par joueur, en ms (0 = chaque coup) |
| `DotInfinite.IncludePets` | 0 | Inclut les DoT des pets / gardiens |
| `DotInfinite.ExcludedSpells` | (commenté) | Liste d'ID de sorts exclus |

## Remarques techniques

- Hooks utilisés (vérifiés sur la branche master d'AzerothCore) : `OnAuraApply`, `OnUnitEnterCombat`, `ModifyPeriodicDamageAurasTick`, `ModifyMeleeDamage`, `ModifySpellDamageTaken`, `OnDamage`, `OnPlayerLogout`, `OnAfterConfigLoad`.
- Pour les attaques normales, `ModifyMeleeDamage` arme un coup et `OnDamage` le valide quand des dégâts sont réellement infligés : un coup raté, esquivé ou paré ne réapplique rien.
- `OnUnitEnterCombat` n'est appelé que pour les créatures dotées d'une IA ; les ticks rattrapent les autres.
- Si le lanceur se déconnecte, l'aura reste mais n'inflige plus de dégâts.
- Un DoT actif maintient la cible en combat tant qu'il tourne.
