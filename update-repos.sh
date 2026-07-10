#!/bin/bash

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

echo "======================================"
echo " AzerothCore Repository Manager"
echo "======================================"

# Vérification git
if [ ! -d ".git" ]; then
    echo "Erreur : ce dossier n'est pas un dépôt git."
    exit 1
fi

# Vérification remote upstream
if ! git remote | grep -q "^upstream$"; then
    echo "ERREUR : remote upstream absent."
    echo "Ajoute-le avec :"
    echo "git remote add upstream URL_DU_REPO_OFFICIEL"
    exit 1
fi


echo
echo "== Mise à jour upstream =="

git fetch upstream


BRANCH=Playerbot

echo "Branche actuelle : $BRANCH"

LOCAL=$(git rev-parse HEAD)
REMOTE=$(git rev-parse upstream/$BRANCH)

if [ "$LOCAL" != "$REMOTE" ]; then
    echo
    echo "⚠️  AzerothCore n'est pas à jour."
    echo
    echo "Différences :"
    git log --oneline HEAD..upstream/$BRANCH

    read -p "Fusionner upstream maintenant ? (y/N) : " REP

    if [[ "$REP" =~ ^[Yy]$ ]]; then
        git merge upstream/$BRANCH
    fi
else
    echo "✓ AzerothCore est à jour."
fi


echo
echo "== Mise à jour des sous-modules existants =="

git submodule sync --recursive
git submodule update --init --recursive --remote


echo
echo "== Analyse des modules =="


if [ -d modules ]; then

    for MODULE in modules/*; do

        [ -d "$MODULE" ] || continue

        NAME=$(basename "$MODULE")

        echo
        echo "--- $NAME ---"

        if [ -d "$MODULE/.git" ]; then

            URL=$(cd "$MODULE" && git remote get-url origin)

            echo "Dépôt Git détecté : $URL"

            if ! git submodule status "$MODULE" >/dev/null 2>&1; then

                echo "Ajout comme sous-module..."

                git submodule add -f "$URL" "$MODULE"

            else

                echo "Déjà sous-module."

            fi

        else

            echo "Dossier sans Git : ignoré."

        fi

    done

fi


echo
echo "== Nettoyage des sous-modules supprimés =="

git submodule status | while read -r HASH PATH REST
do
    if [ ! -d "$PATH" ]; then
        echo "Suppression sous-module absent : $PATH"

        git submodule deinit -f "$PATH" || true
        git rm -f "$PATH" || true
    fi
done


echo
echo "== Etat final =="

git status


echo
echo "======================================"
echo " Terminé"
echo "======================================"

read -p "Commit automatique des changements ? (y/N) : " REP

if [[ "$REP" =~ ^[Yy]$ ]]; then

    git add .gitmodules modules/

    git commit -m "Update AzerothCore modules"

    git push origin "$BRANCH"

    echo "✓ Changements poussés."

fi
