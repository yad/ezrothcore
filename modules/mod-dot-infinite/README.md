# mod-dot-infinite

Module AzerothCore (WoW 3.3.5a) :

1. Les DoT lancés par un joueur ont une durée configurable (120 s par défaut). Avec `DotInfinite.Duration = 0`, ils sont permanents jusqu'à un dispel, la mort de la cible ou un reset/evade.
2. Les DoT posés se propagent aux ennemis qui entrent en combat contre le joueur ou son groupe.

## Installation

1. Copier le dossier `mod-dot-infinite` dans `azerothcore/modules/`.
2. Relancer CMake puis recompiler le worldserver.
3. Copier `conf/mod_dot_infinite.conf.dist` en `mod_dot_infinite.conf` dans `configs/modules/` et l'ajuster.

## Fonctionnement

### Durée
Hook `UnitScript::OnAuraApply` (appelé aussi à chaque rafraîchissement du DoT). Pour chaque aura appliquée, le module vérifie que :

- le lanceur est un joueur (ou son pet / gardien si `DotInfinite.IncludePets = 1`) ;
- l'aura est négative, non canalisée et comporte un effet de dégâts périodiques (`PERIODIC_DAMAGE`, `PERIODIC_DAMAGE_PERCENT`, `PERIODIC_LEECH`) ;
- la cible n'est pas un joueur (sauf `DotInfinite.AffectPlayers = 1`) ;
- le sort n'est pas dans `DotInfinite.ExcludedSpells`.

La durée devient `DotInfinite.Duration` secondes, sans jamais être plus courte que la durée naturelle du sort. Un rafraîchissement remet la durée à ce maximum.

### Propagation
La propagation fonctionne dans les deux sens, pour qu'aucun DoT ne soit oublié quel que soit l'ordre des événements :

- **À chaque DoT posé ou rafraîchi** : le module l'applique aussitôt à tous les ennemis déjà engagés contre le joueur, son pet ou son groupe (liste de combat du core). Un mob qui a rejoint le combat entre ton 1er et ton 3e DoT reçoit donc bien les trois.
- **À chaque nouvel ennemi** (`UnitScript::OnUnitEnterCombat`) : il reçoit les DoT mémorisés encore valides, avec leur date d'expiration, lancés par un joueur du groupe.

Conditions : créature hostile au lanceur, sur la même carte, dans un rayon de `DotInfinite.Spread.Range` yards. Les DoT propagés sont lancés par le joueur (ses stats actuelles), ne relancent pas la propagation en chaîne et ne prolongent pas la mémorisation. Un ennemi qui possède déjà le DoT du même lanceur est ignoré (son timer n'est pas rafraîchi).

La mémorisation est oubliée quand le joueur sort de combat ou se déconnecte.

## Options

| Option | Défaut | Rôle |
|---|---|---|
| `DotInfinite.Enable` | 1 | Active le module |
| `DotInfinite.Duration` | 120 | Durée en secondes (0 = permanent) |
| `DotInfinite.Spread.Enable` | 1 | Propagation aux ennemis qui rejoignent le combat |
| `DotInfinite.Spread.Range` | 60 | Distance max lanceur/ennemi (0 = illimitée) |
| `DotInfinite.AffectPlayers` | 0 | Étend la durée aux cibles joueurs |
| `DotInfinite.IncludePets` | 0 | Inclut les DoT des pets / gardiens |
| `DotInfinite.ExcludedSpells` | vide | Liste d'ID de sorts exclus |

## Remarques

- Les sorts canalisés (Drain de vie, Fouet mental...) sont exclus.
- L'aura peut s'afficher sans timer ou avec un long timer côté client.
- Si le lanceur se déconnecte, l'aura reste mais n'inflige plus de dégâts.
- Un DoT actif maintient la cible en combat tant qu'il tourne.
- Hooks utilisés (vérifiés sur la branche master d'AzerothCore) : `OnAuraApply`, `OnUnitEnterCombat`, `OnPlayerLeaveCombat`, `OnPlayerLogout`, `OnAfterConfigLoad`. `OnUnitEnterCombat` n'est appelé que pour les créatures dotées d'une IA.
