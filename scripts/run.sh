#!/bin/sh
# run.sh — build Redon (if needed) and start the server + browser UI on Linux/macOS.
#
#   ./scripts/run.sh                # in-memory
#   REDON_WAL=redon.wal ./scripts/run.sh   # persist to a WAL
#
# Then open http://127.0.0.1:8080 in your browser. Ctrl-C to stop both.
set -eu

WEB_PORT="${WEB_PORT:-8080}"
REDON_PORT="${REDON_PORT:-6380}"
WAL="${REDON_WAL:-none}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"

if [ ! -x "${BUILD}/redon-server" ] || [ ! -x "${BUILD}/redon-web" ]; then
    echo "Building Redon (Release)..."
    cmake -S "${ROOT}" -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${BUILD}" -j
fi

echo "Starting redon-server on 127.0.0.1:${REDON_PORT} (wal=${WAL}, metrics :9090)..."
"${BUILD}/redon-server" 127.0.0.1 "${REDON_PORT}" 8 "${WAL}" 0 0 --metrics-port 9090 &
SERVER_PID=$!

cleanup() {
    echo
    echo "Stopping..."
    kill "${SERVER_PID}" 2>/dev/null || true
    kill "${WEB_PID:-}" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

sleep 1
echo "Starting redon-web on http://127.0.0.1:${WEB_PORT} ..."
"${BUILD}/redon-web" "${WEB_PORT}" 127.0.0.1 "${REDON_PORT}" &
WEB_PID=$!

# Best-effort: open the browser (no-op if the opener isn't present).
( command -v xdg-open >/dev/null 2>&1 && xdg-open "http://127.0.0.1:${WEB_PORT}" >/dev/null 2>&1 ) || \
( command -v open     >/dev/null 2>&1 && open     "http://127.0.0.1:${WEB_PORT}" >/dev/null 2>&1 ) || true

echo
echo "Redon is running:"
echo "  Web UI   : http://127.0.0.1:${WEB_PORT}"
echo "  Protocol : 127.0.0.1:${REDON_PORT}   (redon-cli ${REDON_PORT})"
echo "  Metrics  : http://127.0.0.1:9090/metrics"
echo
echo "Press Ctrl-C to stop."
wait "${WEB_PID}"
