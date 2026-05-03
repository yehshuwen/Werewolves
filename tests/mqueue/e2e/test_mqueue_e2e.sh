#!/usr/bin/env bash

# End-to-end test for the POSIX Message Queue backend.
# set -x   # debugging
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SERVER_BIN="${BUILD_DIR}/frontends/pipes/werewolf_mqueue_server"
CLIENT_BIN="${BUILD_DIR}/frontends/pipes/werewolf_mqueue_client"

TMP_DIR="$(mktemp -d)"
LOG_DIR="${TMP_DIR}/logs"
mkdir -p "${LOG_DIR}"

GAME_LOG="${LOG_DIR}/game.log"
MODERATOR_LOG="${LOG_DIR}/moderator.log"

# cleanup
cleanup() {
  jobs -p | xargs -r kill 2>/dev/null || true
  rm -f /dev/mqueue/ww_e2e_* 2>/dev/null || true
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

echo "[e2e-mqueue] log dir   : ${LOG_DIR}"
echo "[e2e-mqueue] server bin: ${SERVER_BIN}"
echo "[e2e-mqueue] client bin: ${CLIENT_BIN}"

# verify binaries exist
if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "[e2e-mqueue] ERROR: server binary not found at ${SERVER_BIN}"
  exit 1
fi
if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "[e2e-mqueue] ERROR: client binary not found at ${CLIENT_BIN}"
  exit 1
fi

# launch server
# det-assign: player0=Wolf, player1=Witch, player2=Townperson, player3=Townperson

timeout 40s "${SERVER_BIN}" \
    --players 4 \
    --wolves 1 \
    --lobby 2 \
    --chat 0 \
    --vote 3 \
    --speech 3 \
    --witch 3 \
    --det-assign \
    --no-randomize \
    --game-log "${GAME_LOG}" \
    --moderator-log "${MODERATOR_LOG}" \
    > "${LOG_DIR}/server.stdout" 2> "${LOG_DIR}/server.stderr" & SERVER_PID=$!

sleep 0.5

# player0: Wolf 
{
  sleep 2.5
  echo "vote: player2"
  sleep 6
  echo "vote: player3"
  sleep 10
} | timeout 40s "${CLIENT_BIN}" 0 \
    > "${LOG_DIR}/player0.stdout" 2> "${LOG_DIR}/player0.stderr" & PLAYER0=$!

# player1: Witch
{
  sleep 5.5
  echo "skip"
  sleep 3
  echo "vote: player0"
  sleep 10
} | timeout 40s "${CLIENT_BIN}" 1 \
    > "${LOG_DIR}/player1.stdout" 2> "${LOG_DIR}/player1.stderr" & PLAYER1=$!
  
#player2: Townperson (killed night 1)
{
  sleep 8.5
  echo "I am killed."
  sleep 10
} | timeout 40s "${CLIENT_BIN}" 2 \
    > "${LOG_DIR}/player2.stdout" 2> "${LOG_DIR}/player2.stderr" & PLAYER2=$!

# player3: Townperson
{
  sleep 9
  echo "vote: player0"
  sleep 10
} | timeout 40s "${CLIENT_BIN}" 3 \
    > "${LOG_DIR}/player3.stdout" 2> "${LOG_DIR}/player3.stderr" & PLAYER3=$!

# wait for server to finish
wait "${SERVER_PID}" || {
  echo "[e2e-mqueue] FAIL: server exited with error or timeout"
  echo "--- server stdout ---"
  cat "${LOG_DIR}/server.stdout" || true
  echo "--- server stderr ---"
  cat "${LOG_DIR}/server.stderr" || true
  exit 1
}

wait "${PLAYER0}" || true
wait "${PLAYER1}" || true
wait "${PLAYER2}" || true
wait "${PLAYER3}" || true

# assertions
echo "[e2e-mqueue] Checking assertions..."

check() {
  local pattern="$1"
  local file="$2"
  if grep -q "${pattern}" "${file}"; then
    echo "[e2e-mqueue]   PASS: found '${pattern}'"
  else
    echo "[e2e-mqueue]   FAIL: '${pattern}' not found in ${file}"
    echo "--- file contents ---"
    cat "${file}" || true
    exit 1
  fi
}

# game lifecycle
check ">>> Werewolf game starting <<<"  "${LOG_DIR}/server.stdout"
check "Lobby initialized with 4 players." "${LOG_DIR}/server.stdout"

# night phase — wolf killed player2, witch skipped
check "Night begins"                    "${LOG_DIR}/server.stdout"
check "Night: player2 was killed."      "${LOG_DIR}/server.stdout"
check "Final words from player2: I am killed." "${LOG_DIR}/server.stdout"

# day phase — village lynched player0 (wolf)
check "Day begins"                      "${LOG_DIR}/server.stdout"
check "Day: player0 was lynched."       "${LOG_DIR}/server.stdout"

# game over — village wins
check "Village Win"                     "${LOG_DIR}/server.stdout"
check ">>> Werewolf game ended <<<"     "${LOG_DIR}/server.stdout"

echo "[e2e-mqueue] PASS"
