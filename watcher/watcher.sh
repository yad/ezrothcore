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
SHUTDOWN_PID=""
LAST_AUTH=0
LAST_STATE=""   # avoids spamming the console with the same status every cycle

# --- Logging -----------------------------------------------------

# Colors are disabled when output isn't a terminal (e.g. docker logs
# redirected to a file) so ANSI codes don't pollute the log files.
if [[ -t 1 ]]; then
    C_RESET='\033[0m'
    C_DIM='\033[2m'
    C_INFO='\033[36m'
    C_OK='\033[32m'
    C_WARN='\033[33m'
    C_ERR='\033[31m'
    C_BOLD='\033[1m'
else
    C_RESET='' ; C_DIM='' ; C_INFO='' ; C_OK='' ; C_WARN='' ; C_ERR='' ; C_BOLD=''
fi

_log()
{
    local level="$1" color="$2"; shift 2
    printf "%b%s%b %b[%-5s]%b %s\n" \
        "$C_DIM" "$(date '+%Y-%m-%d %H:%M:%S')" "$C_RESET" \
        "$color" "$level" "$C_RESET" \
        "$*"
}

log_info()  { _log "INFO"  "$C_INFO" "$*"; }
log_ok()    { _log "OK"    "$C_OK"   "$*"; }
log_warn()  { _log "WARN"  "$C_WARN" "$*"; }
log_error() { _log "ERROR" "$C_ERR"  "$*"; }

# --- Docker / MySQL -------------------------------------------------------

worldserver_running()
{
    docker inspect -f '{{.State.Running}}' "$WORLD_CONTAINER" 2>/dev/null | grep -q true
}

start_worldserver()
{
    if worldserver_running; then
        return
    fi

    log_info "Starting worldserver..."

    if docker start "$WORLD_CONTAINER" >/dev/null; then
        START_TIME=$(date +%s)
        LAST_STATE=""

        log_ok "Worldserver started, entering warmup phase (${WARMUP_TIME}s)"
    else
        log_error "Failed to start worldserver"
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
        log_error "Failed to read player count (SQL query failed)"
        echo "-1"
    fi
}

# --- Automatic shutdown ------------------------------------------------

shutdown_countdown()
{
    local remaining=$SHUTDOWN_DELAY

    log_warn "No players connected — automatic shutdown scheduled in ${remaining}s"

    while (( remaining > 0 ))
    do
        sleep 1
        ((remaining--))

        case "$remaining" in
            120|60|30|10|5|4|3|2|1)
                log_warn "Shutting down in ${remaining}s..."
                ;;
        esac
    done

    log_warn "Shutting down worldserver (Docker container)"

    if docker stop "$WORLD_CONTAINER" >/dev/null 2>&1; then
        log_ok "Worldserver stopped successfully"
    else
        log_error "Failed to stop worldserver"
    fi
}

# Returns true if a countdown is currently running (based on the actual
# PID state, not a shared boolean, which doesn't survive the "&" fork).
shutdown_in_progress()
{
    [[ -n "$SHUTDOWN_PID" ]] && kill -0 "$SHUTDOWN_PID" 2>/dev/null
}

schedule_shutdown()
{
    if shutdown_in_progress; then
        return
    fi

    shutdown_countdown &

    SHUTDOWN_PID=$!
}

cancel_shutdown()
{
    if ! shutdown_in_progress; then
        SHUTDOWN_PID=""
        return
    fi

    kill "$SHUTDOWN_PID" 2>/dev/null
    wait "$SHUTDOWN_PID" 2>/dev/null

    log_ok "Automatic shutdown cancelled (activity detected)"

    SHUTDOWN_PID=""
}

# --- Startup --------------------------------------------------------

printf "%b" "$C_BOLD"
echo "============================================================"
echo " Worldserver Watcher — automatic monitoring and management"
echo "============================================================"
printf "%b" "$C_RESET"
log_info "Monitored container: ${WORLD_CONTAINER}"
log_info "Check interval: ${CHECK_INTERVAL}s | Warmup: ${WARMUP_TIME}s | Shutdown delay: ${SHUTDOWN_DELAY}s"

if worldserver_running; then
    START_TIME=$(date +%s)
    log_ok "Worldserver already running at watcher startup"
else
    log_info "Worldserver currently stopped — waiting for a connection"
fi

(
    while [ ! -f "$AUTH_LOG" ]; do
        log_warn "Log file not found, retrying: $AUTH_LOG"
        sleep 5
    done

    log_ok "Authentication monitoring active ($AUTH_LOG)"

    tail -Fn0 "$AUTH_LOG" | while read -r line; do

        if echo "$line" | grep -q "successfully authenticated"; then

            NOW=$(date +%s)

            if (( NOW - LAST_AUTH < AUTH_COOLDOWN )); then
                continue
            fi

            LAST_AUTH=$NOW

            log_ok "Authentication detected → waking up worldserver"

            start_worldserver
            cancel_shutdown
        fi

    done

) &

while true
do
    if ! worldserver_running; then
        if [[ "$LAST_STATE" != "stopped" ]]; then
            log_info "Idle — worldserver is stopped"
            LAST_STATE="stopped"
        fi
        sleep "$CHECK_INTERVAL"
        continue
    fi

    NOW=$(date +%s)

    if (( START_TIME > 0 )) && (( NOW - START_TIME < WARMUP_TIME )); then

        REMAINING=$((WARMUP_TIME - (NOW - START_TIME)))

        log_info "Warmup in progress (${REMAINING}s remaining)"

        sleep "$CHECK_INTERVAL"
        continue
    fi

    PLAYERS=$(get_real_players)

    if (( PLAYERS < 0 )); then
        sleep "$CHECK_INTERVAL"
        continue
    fi

    if (( PLAYERS > 0 )); then
        if [[ "$LAST_STATE" != "active" ]]; then
            log_ok "${PLAYERS} player(s) connected — server kept alive"
            LAST_STATE="active"
        fi
        cancel_shutdown
    else
        if [[ "$LAST_STATE" != "idle" ]]; then
            log_warn "No players connected — entering idle mode"
            LAST_STATE="idle"
        fi
        schedule_shutdown
    fi

    sleep "$CHECK_INTERVAL"
done