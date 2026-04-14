#!/usr/bin/env bash

# debug
# set -x
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
# echo $ROOT_DIR
BUILD_DIR="${ROOT_DIR}/build"
SERVER_BIN="${BUILD_DIR}/frontends/pipes/werewolf_pipe_server"
CLIENT_BIN="${BUILD_DIR}/frontends/pipes/werewolf_pipe_client"

TMP_DIR="$(mktemp -d)"
PIPE_ROOT="${TMP_DIR}/pipes"
LOG_DIR="${TMP_DIR}/logs"

mkdir -p "${PIPE_ROOT}" "${LOG_DIR}"

GAME_LOG="${LOG_DIR}/game.log"
MODERATOR_LOG="${LOG_DIR}/moderator.log"

# cleanup
cleanup() {
    # find all bg processes, pass to xargs and then kill
    jobs -p | xargs -r kill 2>/dev/null || true
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

echo "[e2e] pipe root: ${PIPE_ROOT}"
echo "[e2e] log dir: ${LOG_DIR}"
echo "[e2e] server: ${SERVER_BIN}"
echo "[e2e] client: ${CLIENT_BIN}"

# launch server, timeout after 20s
# put this process to bg and store the PID to SERVER_PID
timeout 20s "${SERVER_BIN}" \
    --pipe-root "${PIPE_ROOT}" \
    --create-fifos \
    --players 4 \
    --wolves 1 \
    --lobby 1 \
    --chat 0 \
    --vote 1 \
    --speech 1 \
    --witch 1 \
    --det-assign \
    --game-log "${GAME_LOG}" \
    --moderator-log "${MODERATOR_LOG}" \
    > "${LOG_DIR}/server.stdout" 2> "${LOG_DIR}/server.stderr" & SERVER_PID=$!

sleep 0.5

# player0: wolf
{
    sleep 1
    echo "vote: player2"
    sleep 2
    echo "vote: player3"
} | timeout 20s "${CLIENT_BIN}" 0 --pipe-root "${PIPE_ROOT}" > "${LOG_DIR}/player0.stdout" 2> "${LOG_DIR}/player0.stderr" & PLAYER0=$!

# player1: witch
{
    sleep 2
    echo "skip"
    sleep 2
    echo "vote: player3"
} | timeout 20s "${CLIENT_BIN}" 1 --pipe-root "${PIPE_ROOT}" > "${LOG_DIR}/player1.stdout" 2> "${LOG_DIR}/player1.stderr" & PLAYER1=$!

# player2: town
{
    sleep 3
    echo "I am killed."
} | timeout 20s "${CLIENT_BIN}" 2 --pipe-root "${PIPE_ROOT}" > "${LOG_DIR}/player2.stdout" 2> "${LOG_DIR}/player2.stderr" & PLAYER2=$!

# player3: town
{
    sleep 4
    echo "I am lynched."
} | timeout 20s "${CLIENT_BIN}" 3 --pipe-root "${PIPE_ROOT}" > "${LOG_DIR}/player3.stdout" 2> "${LOG_DIR}/player3.stderr" & PLAYER3=$!

wait "${SERVER_PID}" || {
    echo "[e2e] server failed"
exit 1
}

wait "${PLAYER0}" || true
wait "${PLAYER1}" || true
wait "${PLAYER2}" || true
wait "${PLAYER3}" || true

# Assertions on server stdout first, because your Game currently prints there.
grep -q ">>> Werewolf game starting <<<" "${LOG_DIR}/server.stdout"
grep -q "Lobby initialized with 4 players." "${LOG_DIR}/server.stdout"
grep -q "Night starts" "${LOG_DIR}/server.stdout"
grep -q "Night: player2 was killed." "${LOG_DIR}/server.stdout"
grep -q "Final words from player2: I am killed." "${LOG_DIR}/server.stdout"
grep -q "Day starts" "${LOG_DIR}/server.stdout"
grep -q "Day: player3 was lynched." "${LOG_DIR}/server.stdout"
grep -q "Final words from player3: I am lynched." "${LOG_DIR}/server.stdout"

echo "[e2e] PASS"
