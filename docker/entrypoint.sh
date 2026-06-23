#!/bin/sh
# entrypoint.sh — start the Redon server and the browser gateway together.
#
# Environment knobs (all optional):
#   REDON_DISK    path to an on-disk database file (enables persistence)
#   REDON_WORKERS server worker-thread count (default 8)
#   REDON_WEB_HOST interface the web UI binds to (set to 0.0.0.0 in the image)
set -eu

WORKERS="${REDON_WORKERS:-8}"

# Persistence: if REDON_DISK is set, use the on-disk engine; otherwise in-memory.
if [ -n "${REDON_DISK:-}" ]; then
    DISK_ARGS="--disk ${REDON_DISK}"
else
    DISK_ARGS=""
fi

# Start the Redon data server on all interfaces, with Prometheus metrics on 9090.
# shellcheck disable=SC2086
redon-server 0.0.0.0 6380 "${WORKERS}" none 0 0 --metrics-port 9090 ${DISK_ARGS} &
SERVER_PID=$!

# When this script is told to stop, take the server down with it.
trap 'kill "${SERVER_PID}" 2>/dev/null || true' INT TERM

# Give the server a moment to bind, then run the web gateway in the foreground
# (it bridges browser -> 127.0.0.1:6380 inside the container).
sleep 1
exec redon-web 8080 127.0.0.1 6380
