#!/bin/bash

set -u

WORLD_CONTAINER="ac-worldserver"
AUTH_LOG="/logs/Auth.log"

MYSQL_HOST="ac-database"
MYSQL_USER="root"
MYSQL_PASSWORD="${MYSQL_ROOT_PASSWORD}"

CHECK_INTERVAL=10
WARMUP_TIME=180
SHUTDOWN_DELAY=180
AUTH_COOLDOWN=30

START_TIME=0
SHUTDOWN_SCHEDULED=false
SHUTDOWN_PID=""
LAST_AUTH=0

log()
{
    echo "$(date '+%Y-%m-%d %H:%M:%S') : $*"
}

worldserver_running()
{
    docker inspect -f '{{.State.Running}}' "$WORLD_CONTAINER" 2>/dev/null | grep -q true
}

start_worldserver()
{
    if worldserver_running; then
        return
    fi

    log "Démarrage worldserver"

    if docker start "$WORLD_CONTAINER" >/dev/null; then
        START_TIME=$(date +%s)
        SHUTDOWN_SCHEDULED=false
        SHUTDOWN_PID=""

        log "Worldserver démarré"
    else
        log "Erreur démarrage worldserver"
    fi
}

get_real_players()
{
    local result

    result=$(
        mysql \
            --protocol=tcp \
            -h "$MYSQL_HOST" \
            -u "$MYSQL_USER" \
            -p"$MYSQL_PASSWORD" \
            -N -B \
            -e "
                SELECT COUNT(*)
                FROM acore_auth.account a
                INNER JOIN acore_characters.characters c
                    ON a.id = c.account
                WHERE GREATEST(a.online, c.online) = 1
                AND a.username NOT LIKE 'RNDBOT%'
                AND a.username <> 'AHBOT';
            " 2>/dev/null
    )

    if [[ "$result" =~ ^[0-9]+$ ]]; then
        echo "$result"
    else
        log "Erreur lecture joueurs SQL"
        echo "-1"
    fi
}

shutdown_countdown()
{
    local remaining=$SHUTDOWN_DELAY

    log "Aucun joueur connecté. Arrêt automatique du serveur dans ${remaining} secondes."

    while (( remaining > 0 ))
    do
        sleep 1
        ((remaining--))

        case "$remaining" in
            120|60|30|10|5|4|3|2|1)
                log "Arrêt du serveur dans ${remaining} seconde(s)."
                ;;
        esac
    done

    log "Arrêt du serveur."

    log "Arrêt du conteneur Docker"

    docker stop "$WORLD_CONTAINER" >/dev/null 2>&1

    SHUTDOWN_SCHEDULED=false
    SHUTDOWN_PID=""
}

schedule_shutdown()
{
    if $SHUTDOWN_SCHEDULED; then
        return
    fi

    log "Démarrage du countdown (${SHUTDOWN_DELAY}s)"

    shutdown_countdown &

    SHUTDOWN_PID=$!
    SHUTDOWN_SCHEDULED=true
}

cancel_shutdown()
{
    if ! $SHUTDOWN_SCHEDULED; then
        return
    fi

    if [[ -n "$SHUTDOWN_PID" ]] && kill -0 "$SHUTDOWN_PID" 2>/dev/null; then
        kill "$SHUTDOWN_PID" 2>/dev/null
        wait "$SHUTDOWN_PID" 2>/dev/null
    fi

    log "Arrêt automatique annulé."

    SHUTDOWN_PID=""
    SHUTDOWN_SCHEDULED=false

    log "Countdown annulé"
}

log "Worldserver watcher démarré"

if worldserver_running; then
    START_TIME=$(date +%s)
    log "Worldserver déjà actif"
fi

(
    while [ ! -f "$AUTH_LOG" ]; do
        log "Attente $AUTH_LOG"
        sleep 5
    done

    tail -Fn0 "$AUTH_LOG" | while read -r line; do

        if echo "$line" | grep -q "successfully authenticated"; then

            NOW=$(date +%s)

            if (( NOW - LAST_AUTH < AUTH_COOLDOWN )); then
                continue
            fi

            LAST_AUTH=$NOW

            log "Authentification détectée"

            start_worldserver
            cancel_shutdown
        fi

    done

) &

while true
do
    if ! worldserver_running; then
        sleep "$CHECK_INTERVAL"
        continue
    fi

    NOW=$(date +%s)

    if (( START_TIME > 0 )) && (( NOW - START_TIME < WARMUP_TIME )); then

        REMAINING=$((WARMUP_TIME - (NOW - START_TIME)))

        log "Warmup actif (${REMAINING}s restantes)"

        sleep "$CHECK_INTERVAL"
        continue
    fi

    PLAYERS=$(get_real_players)

    if (( PLAYERS < 0 )); then
        sleep "$CHECK_INTERVAL"
        continue
    fi

    if (( PLAYERS > 0 )); then
        log "Joueurs connectés = $PLAYERS, keep alive"
        cancel_shutdown
    else
        log "Joueurs connectés = $PLAYERS, shutdown"
        schedule_shutdown
    fi

    sleep "$CHECK_INTERVAL"
done
