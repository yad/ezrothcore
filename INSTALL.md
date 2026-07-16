# 🚀 Installation

## 📋 Prérequis

*(Sous Windows, utiliser les versions x64.)*

Installer les outils suivants :

- 🐧 **WSL**
  https://learn.microsoft.com/fr-fr/windows/wsl/install

- 🌱 **Git**
  https://git-scm.com/install/windows

- 🐳 **Docker Desktop**
  https://docs.docker.com/desktop/setup/install/windows-install/

> 📝 Pour Docker :
> - Installer avec le support **WSL** activé.
> - Choisir la version **Community** (gratuite, usage personnel).
> - La création d'un compte n'est pas nécessaire pour l'installation ni pour le lancement.

---

## ⚙️ Installation

Vérifier que **🐳 Docker Desktop est démarré**.

Ouvrir un terminal.
*(Sous Windows, l'installation de Git fournit également `Git Bash`.)*

Cloner le projet :

```bash
git clone --recurse-submodules https://github.com/yad/ezrothcore.git

cd ezrothcore

git clone https://github.com/yad/ezrothcore_client_data.git client_data

rm -rf env/dist/etc

git clone https://github.com/yad/ezrothcore_conf.git env/dist/etc

docker compose up --build --detach
```

Options utilisées :

- 🔨 `--build` : compile le serveur. Cette étape peut être longue.
- 🚀 `--detach` : lance les composants Docker en arrière-plan et rend la main au terminal dès que possible.

---

# 🛠️ Troubleshooting et erreurs connues

## 🐳 Docker non démarré

Vérifier via l'interface **Docker Desktop** que Docker est bien lancé.

Sinon, l'erreur suivante peut apparaître :

```
failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine; check if the path is correct and if the daemon is running: open //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified.
```

---

## 🔎 Vérifier l'état des composants

Utiliser :

```bash
docker ps -a --format "table {{.Names}}\t{{.State}}\t{{.Status}}"
```

Résultats attendus :

- ✅ `Up` : OK
- ✅ `Exited (0)` : OK

Si un composant est en erreur (`Exited` avec un code différent de `0`), consulter ses logs.

---

## 📜 Vérifier les logs des composants

Utiliser :

```bash
docker logs -f <nom_du_composant>
```

L'option `-f` (*follow*) permet de suivre les logs en temps réel.

---

### 🗄️ Base de données

```bash
docker logs -f ac-database
```

Si le message suivant apparaît :

```
[Server] /usr/sbin/mysqld: ready for connections. Version: '8.4.10' socket: '/var/run/mysqld/mysqld.sock'
```

→ ✅ OK

---

### 📦 Initialisation des données client

```bash
docker logs -f ac-client-data-init
```

Si le message suivant apparaît :

```
bye
```

→ ✅ OK

*(Ce composant est désactivé volontairement, il n'est plus utilisé.)*

---

### 🗃️ Import des bases de données

```bash
docker logs -f ac-db-import
```

Pour les trois bases (**Auth**, **Character**, **World**), le message attendu est :

```
database is up-to-date
```

→ ✅ OK

---

### 🔐 Auth Server

```bash
docker logs -f ac-authserver
```

Si le message suivant apparaît :

```
Updating realm "AzerothCore" at 127.0.0.1:8085
```

→ ✅ OK

*(Le niveau de verbosité est volontairement élevé pour le watcher.)*

---

### 🌍 World Server

```bash
docker logs -f ac-worldserver
```

Si le message suivant apparaît :

```
WORLD: World Initialized In ...
```

→ ✅ OK

---

### 👀 World Server Watcher

```bash
docker logs -f ac-worldserver-watcher
```

Si le message suivant apparaît :

```
Worldserver watcher démarré
```

→ ✅ OK

Le watcher :
- 🛑 arrête automatiquement le serveur lorsqu'aucun joueur n'est connecté ;
- ▶️ relance le serveur lorsqu'un joueur se connecte.

---

# ▶️ Démarrer le serveur (si nécessaire)

Docker démarre normalement automatiquement les composants.

Si un démarrage manuel est nécessaire :

```bash
docker compose up -d
```

ℹ️ Ici, il n'est plus nécessaire d'utiliser `--build` : le serveur a déjà été compilé.

---

# 🔄 Mise à jour

🚧 TODO

---

# ⚠️ Important

## 🏦 AuctionHouseBot

Si l'erreur suivante apparaît :

```
No character GUIDs found when looking up values from AuctionHouseBot.GUIDs from the character database 'characters.guid'.
```

Il faut :

1. 👤 Créer un compte dédié.
   - ❌ Ne pas utiliser un compte provenant de **PlayerBot**.

2. 🧙 Créer un personnage qui sera utilisé par l'**AuctionHouseBot**.

3. 🔢 Récupérer l'identifiant (`GUID`) du personnage.

4. 📝 Déclarer cet identifiant dans :

```
env/dist/etc/modules/mod_ahbot.conf
```