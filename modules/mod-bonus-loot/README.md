# mod-bonus-loot

Ce module attribue une piece d'armure bonus quand un joueur recupere un objet depuis une creature ou un coffre.

La piece est choisie dans la table de butin du template de la source. Elle est compatible avec la classe du destinataire et sa qualite est strictement `Uncommon`, `Rare` ou `Epic`. En groupe, le joueur et les membres humains en ligne recoivent chacun leur propre piece adaptee. Les playerbots sont exclus par defaut.

Le bonus est ajoute directement a l'inventaire. Si celui-ci est plein, la piece est envoyee par courrier par le Postmaster. Le loot original n'est jamais modifie, supprime ou remplace.

## Installation

1. Activer le module puis relancer la configuration et la compilation.
2. Copier `mod-bonus-loot.conf.dist` dans la configuration des modules si une personnalisation est necessaire.

## Configuration

- `BonusLoot.Enable` active le module.
- `BonusLoot.ExcludeBots` ignore les personnages de mod-playerbots.
- `BonusLoot.StrictArmorPreference` applique la specialisation d'armure de la classe, avec repli vers la proficiency inferieure si necessaire.
- `BonusLoot.Announce` affiche un message lors de l'attribution.
