# The browser UI (`redon-web`) — explained

The nine phases build a real database you talk to over **raw TCP**. But a web
browser can't open a raw TCP socket — it only speaks **HTTP**. So to let *anyone*
use Redon from a web page (no terminal, no `redon-cli`), we add a small bridge:
`redon-web`.

```
   browser  ──HTTP──▶  redon-web  ──TCP line──▶  redon-server
            ◀─JSON──             ◀─reply line──
```

`redon-web` is a tiny HTTP server that does two jobs:

1. **Serves the page.** `GET /` returns a single self-contained HTML page (CSS and
   JS inline — no external files). That page is the UI: an input box, quick
   buttons, a scrolling output log, and a little stats strip.
2. **Bridges commands.** When you run a command in the page, the JS does
   `POST /cmd` with the command text. `redon-web` opens a short TCP connection to
   `redon-server`, sends that one line, reads the one reply line, and returns it
   as JSON: `{"ok":true,"reply":"PONG"}`.

That's the whole idea. Everything else is making it robust.

## Why a separate program (and not a feature of the server)?

Keeping the gateway separate mirrors how real systems are built — a **stateless
edge/proxy** in front of the data tier:

- The server stays a pure database that speaks one protocol. The web concern
  lives elsewhere.
- You can run *one* gateway in front of a *router* (Phase 7), so the browser
  reaches a whole sharded cluster through a single page.
- `redon-web` depends only on `net.h` + threads — not on the storage engine — so
  it's small and can't corrupt anything.

## How it works, in code ([web.cpp](../src/web.cpp))

**A small thread pool of acceptors.** `start()` binds the port and spawns N worker
threads that all call `accept()` on the same listening socket; the OS hands each
new connection to whichever worker is free. Shutdown closes the listening socket,
which makes every blocked `accept()` return so the threads can be joined — the
same interruptible-accept trick the metrics endpoint uses.

**Real HTTP request parsing.** A browser request can arrive in several TCP reads,
so the gateway reads until it sees the `\r\n\r\n` that ends the headers, parses the
request line (`POST /cmd HTTP/1.1`) and the `Content-Length`, then keeps reading
until it has the whole body. (The minimal metrics endpoint could get away with a
single `recv`; a gateway that accepts POST bodies cannot.)

**Bridging is just the router pattern.** `forward_to_redon()` is `connect_tcp` →
`send_all(line + "\n")` → `recv_line` → close, with a timeout — exactly how the
Phase 7 router forwards to a shard. One browser command = one short-lived TCP
request to Redon.

**Two small but important safety details:**

- **Only the first line of the POST body is used.** If a request body contained
  `SET a 1\nDEL b`, taking only the first line stops the browser from smuggling a
  *second* command into one request — the web equivalent of the key-parsing
  discipline the rest of the project cares about.
- **Replies are JSON-escaped.** A stored value can contain quotes or backslashes;
  `json_escape` makes sure that can't break the JSON (or inject into the page).

**Honest limitations:**

- The gateway is **unauthenticated** — anyone who can reach the port can run
  commands. It binds `127.0.0.1` by default for that reason; `REDON_WEB_HOST=0.0.0.0`
  opens it up (the Docker image sets this so the published port works). On a public
  host you'd put real auth / a reverse proxy in front.
- It opens a **fresh TCP connection per command**. That's fine for a demo UI; a
  high-traffic gateway would pool connections.
- It's a **subset of HTTP** — enough to serve one page and accept POSTs, not a
  general web server.

## Run it

```sh
# build, start server + UI, open the browser (one command):
scripts/run.ps1        # Windows
./scripts/run.sh       # Linux/macOS

# or directly:
redon-server 127.0.0.1 6380 8 none 0 0 --metrics-port 9090
redon-web    8080 127.0.0.1 6380        # open http://127.0.0.1:8080
```

`redon-web [web_port] [redon_host] [redon_port] [workers]` — defaults `8080`,
`127.0.0.1`, `6380`, `8`.

## Endpoints

| Method & path | What it does |
|---|---|
| `GET /` | the web UI (HTML page) |
| `POST /cmd` | run one command; body is the command line, reply is JSON |
| `GET /healthz` | liveness of the gateway itself (`ok`) |

**New ideas this adds:** a *gateway/proxy* tier, *protocol translation*
(HTTP ⇄ Redon's line protocol), real (if minimal) *HTTP request parsing*, and the
security reflex of *not trusting client input* (single-line commands, escaped
output). It's the piece that turns "a database I can use from a terminal" into
"a database anyone can open in a browser."
