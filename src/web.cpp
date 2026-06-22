// web.cpp — implementation of the browser gateway (see web.h).
#include "web.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

namespace redon {
namespace {

constexpr std::size_t kMaxRequest = 1u << 20;   // 1 MiB cap on a request
constexpr int kRedonTimeoutMs = 5000;           // per-command send/recv timeout
constexpr std::size_t kMaxReply = 256 * 1024;   // a single reply line is bounded

bool send_all(net::socket_t sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(sock, data.data() + static_cast<int>(sent),
                       static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Read one '\n'-terminated line (a Redon reply). Trailing '\r' is stripped.
bool recv_line(net::socket_t sock, std::string* line) {
    line->clear();
    char chunk[4096];
    for (;;) {
        int n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (chunk[i] == '\n') {
                if (!line->empty() && line->back() == '\r') {
                    line->pop_back();
                }
                return true;
            }
            line->push_back(chunk[i]);
            if (line->size() > kMaxReply) {
                return false;
            }
        }
    }
}

std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// Parse the Content-Length header out of a raw header block (case-insensitive).
std::size_t parse_content_length(const std::string& headers) {
    std::istringstream is(headers);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (ascii_lower(line.substr(0, colon)) == "content-length") {
            try {
                long long v = std::stoll(line.substr(colon + 1));
                return v > 0 ? static_cast<std::size_t>(v) : 0;
            } catch (const std::exception&) {
                return 0;
            }
        }
    }
    return 0;
}

// Take only the first line of `s` (so a browser can't smuggle a second command
// by embedding a newline in the body).
std::string first_line(const std::string& s) {
    std::size_t nl = s.find_first_of("\r\n");
    std::string out = nl == std::string::npos ? s : s.substr(0, nl);
    // trim surrounding ASCII whitespace
    std::size_t a = out.find_first_not_of(" \t");
    std::size_t b = out.find_last_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    return out.substr(a, b - a + 1);
}

// Escape a string for embedding inside a JSON string literal.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string http_response(const std::string& status, const std::string& ctype,
                          const std::string& body) {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << "\r\n"
       << "Content-Type: " << ctype << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return os.str();
}

// The single-page web app, served at GET /. Self-contained: inline CSS + JS,
// no external assets, talks only to this gateway's own /cmd endpoint.
const char* const kIndexPage = R"PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Redon — distributed key-value store</title>
<style>
  :root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--muted:#8b949e;
        --accent:#58a6ff;--ok:#3fb950;--err:#f85149;--num:#d29922}
  *{box-sizing:border-box}
  body{margin:0;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
       background:var(--bg);color:var(--fg)}
  header{padding:18px 22px;border-bottom:1px solid var(--border);display:flex;
         align-items:center;gap:14px;flex-wrap:wrap}
  .logo{font-size:22px;font-weight:700;letter-spacing:.5px}
  .logo span{color:var(--accent)}
  .tag{color:var(--muted);font-size:13px}
  .status{margin-left:auto;font-size:13px;color:var(--muted)}
  .dot{width:10px;height:10px;border-radius:50%;background:var(--err);
       display:inline-block;margin-right:6px;vertical-align:middle}
  .dot.up{background:var(--ok)}
  main{max-width:900px;margin:0 auto;padding:22px}
  .stats{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px}
  .stat{background:var(--panel);border:1px solid var(--border);border-radius:8px;
        padding:8px 12px;font-size:12px;min-width:92px;color:var(--muted)}
  .stat b{display:block;font-size:16px;color:var(--fg);margin-top:2px}
  #out{background:#010409;border:1px solid var(--border);border-radius:10px;
       padding:14px;height:340px;overflow:auto;font-size:13px;line-height:1.55;
       white-space:pre-wrap;word-break:break-word}
  .line{margin:2px 0}
  .cmd{color:var(--accent)} .ok{color:var(--ok)} .err{color:var(--err)}
  .num{color:var(--num)} .muted{color:var(--muted)}
  form{display:flex;gap:8px;margin-top:12px}
  input[type=text]{flex:1;background:var(--panel);border:1px solid var(--border);
       color:var(--fg);border-radius:8px;padding:11px 13px;font:inherit}
  input[type=text]:focus{outline:none;border-color:var(--accent)}
  button{background:var(--accent);color:#0d1117;border:0;border-radius:8px;
         padding:0 18px;font:inherit;font-weight:700;cursor:pointer}
  button:hover{opacity:.9}
  .chips{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
  .chip{background:var(--panel);border:1px solid var(--border);color:var(--muted);
        border-radius:20px;padding:6px 12px;font-size:12px;cursor:pointer}
  .chip:hover{border-color:var(--accent);color:var(--fg)}
  details{margin-top:20px;color:var(--muted);font-size:13px}
  code{background:var(--panel);padding:1px 5px;border-radius:4px;color:var(--fg)}
  footer{text-align:center;color:var(--muted);font-size:12px;padding:20px}
</style>
</head>
<body>
<header>
  <div class="logo">Re<span>don</span></div>
  <div class="tag">a distributed key-value store, built from scratch in C++</div>
  <div class="status"><span id="dot" class="dot"></span><span id="statusText">connecting…</span></div>
</header>
<main>
  <div class="stats" id="stats"></div>
  <div id="out"></div>
  <form id="form" autocomplete="off">
    <input id="cmd" type="text" placeholder="type a command, e.g.  SET name Saurabh" autofocus>
    <button type="submit">Run</button>
  </form>
  <div class="chips" id="chips"></div>
  <details>
    <summary>Commands &amp; how this works</summary>
    <p><code>SET key value</code> · <code>GET key</code> · <code>DEL key</code> ·
       <code>EXISTS key</code> · <code>PING</code> · <code>INFO</code></p>
    <p>Each command is sent as one TCP line to the Redon server and returns one
       reply line. This page is served by <code>redon-web</code>, a small gateway
       that bridges your browser to the Redon TCP server.</p>
  </details>
</main>
<footer>Redon · a 9-phase distributed key-value store</footer>
<script>
const out=document.getElementById('out');
function append(text,cls){const d=document.createElement('div');
  d.className='line '+(cls||'');d.textContent=text;out.appendChild(d);
  out.scrollTop=out.scrollHeight;}
function classify(r){
  if(r==='OK'||r==='PONG')return 'ok';
  if(r.startsWith('ERR'))return 'err';
  if(r==='(nil)')return 'muted';
  if(r.startsWith('(integer)'))return 'num';
  return '';}
async function run(cmd){
  cmd=(cmd||'').trim();if(!cmd)return;
  append('> '+cmd,'cmd');
  try{
    const r=await fetch('/cmd',{method:'POST',body:cmd});
    const j=await r.json();
    if(j.ok)append(j.reply,classify(j.reply));
    else append('✗ '+(j.error||'error'),'err');
  }catch(e){append('✗ network error','err');}
  refreshStats();}
document.getElementById('form').addEventListener('submit',e=>{
  e.preventDefault();const i=document.getElementById('cmd');run(i.value);i.value='';});
const chips=[['PING','PING'],
  ['SET demo','SET demo:greeting hello from the browser'],
  ['GET demo','GET demo:greeting'],
  ['EXISTS demo','EXISTS demo:greeting'],
  ['DEL demo','DEL demo:greeting'],
  ['INFO','INFO']];
const chipBox=document.getElementById('chips');
chips.forEach(c=>{const b=document.createElement('span');b.className='chip';
  b.textContent=c[0];b.onclick=()=>run(c[1]);chipBox.appendChild(b);});
const dot=document.getElementById('dot'),statusText=document.getElementById('statusText'),
      stats=document.getElementById('stats');
function showStats(info){
  const p={};info.split(' ').forEach(kv=>{const i=kv.indexOf('=');
    if(i>0)p[kv.slice(0,i)]=kv.slice(i+1);});
  const want=[['keys','keys'],['commands','commands'],['hit_rate','hit rate'],
              ['clients','clients'],['uptime_s','uptime (s)']];
  stats.innerHTML='';
  want.forEach(w=>{if(p[w[0]]!==undefined){const d=document.createElement('div');
    d.className='stat';d.innerHTML=w[1]+'<b>'+p[w[0]]+'</b>';stats.appendChild(d);}});}
async function refreshStats(){
  try{
    const r=await fetch('/cmd',{method:'POST',body:'INFO'});const j=await r.json();
    if(j.ok&&j.reply.indexOf('uptime_s=')>=0){
      showStats(j.reply);dot.className='dot up';statusText.textContent='connected to Redon';}
    else{dot.className='dot';statusText.textContent='Redon unreachable';}
  }catch(e){dot.className='dot';statusText.textContent='gateway down';}}
append('Welcome to Redon. Type a command below or click a chip to begin.','muted');
refreshStats();setInterval(refreshStats,3000);
</script>
</body>
</html>
)PAGE";

}  // namespace

WebGateway::WebGateway(std::string bind_host, std::uint16_t web_port,
                       std::string redon_host, std::uint16_t redon_port,
                       int workers)
    : bind_host_(std::move(bind_host)),
      web_port_(web_port),
      redon_host_(std::move(redon_host)),
      redon_port_(redon_port),
      workers_(workers < 1 ? 1 : workers) {}

WebGateway::~WebGateway() { stop(); }

bool WebGateway::start() {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ == net::kInvalidSocket) {
        return false;
    }
    int reuse = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(web_port_);
    if (::inet_pton(AF_INET, bind_host_.c_str(), &addr.sin_addr) != 1 ||
        ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_, 64) != 0) {
        net::close_socket(listen_);
        listen_ = net::kInvalidSocket;
        return false;
    }
    running_.store(true);
    for (int i = 0; i < workers_; ++i) {
        threads_.emplace_back([this] { worker_loop(); });
    }
    return true;
}

void WebGateway::stop() {
    bool was = running_.exchange(false);
    if (listen_ != net::kInvalidSocket) {
        net::close_socket(listen_);  // unblocks accept() in every worker
        listen_ = net::kInvalidSocket;
    }
    if (was) {
        for (std::thread& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }
}

void WebGateway::wait() {
    for (std::thread& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void WebGateway::worker_loop() {
    while (running_.load()) {
        net::socket_t client = ::accept(listen_, nullptr, nullptr);
        if (client == net::kInvalidSocket) {
            if (!running_.load()) {
                break;  // shutting down: listen socket was closed
            }
            continue;
        }
        handle_connection(client);
    }
}

void WebGateway::handle_connection(net::socket_t client) {
    net::SocketCloser guard(client);
    net::set_recv_timeout(client, 10);

    // Read until the end of the HTTP headers.
    std::string buf;
    char chunk[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        int n = ::recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            return;
        }
        buf.append(chunk, static_cast<std::size_t>(n));
        if (buf.size() > kMaxRequest) {
            send_all(client, http_response("413 Payload Too Large", "text/plain",
                                           "request too large"));
            return;
        }
    }

    const std::string head = buf.substr(0, header_end);
    std::string method, path;
    {
        std::istringstream rl(head);
        rl >> method >> path;  // "GET /path HTTP/1.1"
    }

    // Read the rest of the body, if any (POST /cmd).
    const std::size_t content_length = parse_content_length(head);
    std::string body = buf.substr(header_end + 4);
    while (body.size() < content_length) {
        int n = ::recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            break;
        }
        body.append(chunk, static_cast<std::size_t>(n));
        if (body.size() > kMaxRequest) {
            break;
        }
    }
    if (body.size() > content_length) {
        body.resize(content_length);
    }

    send_all(client, route(method, path, body));
}

std::string WebGateway::route(const std::string& method, const std::string& path,
                              const std::string& body) {
    if (method == "OPTIONS") {
        return http_response("204 No Content", "text/plain", "");
    }
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        return http_response("200 OK", "text/html; charset=utf-8", kIndexPage);
    }
    if (method == "GET" && path == "/healthz") {
        return http_response("200 OK", "text/plain", "ok");
    }
    if (method == "GET" && path == "/favicon.ico") {
        return http_response("204 No Content", "text/plain", "");
    }
    if (method == "POST" && path == "/cmd") {
        const std::string cmd = first_line(body);
        if (cmd.empty()) {
            return http_response("400 Bad Request", "application/json",
                                 "{\"ok\":false,\"error\":\"empty command\"}");
        }
        std::string reply;
        if (!forward_to_redon(cmd, &reply)) {
            const std::string err = "Redon server unreachable at " + redon_host_ +
                                    ":" + std::to_string(redon_port_);
            return http_response(
                "502 Bad Gateway", "application/json",
                "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}");
        }
        return http_response(
            "200 OK", "application/json",
            "{\"ok\":true,\"reply\":\"" + json_escape(reply) + "\"}");
    }
    return http_response("404 Not Found", "text/plain", "not found");
}

bool WebGateway::forward_to_redon(const std::string& line, std::string* reply) {
    net::socket_t sock = net::connect_tcp(redon_host_, redon_port_);
    if (sock == net::kInvalidSocket) {
        return false;
    }
    net::SocketCloser guard(sock);
    net::set_io_timeout_ms(sock, kRedonTimeoutMs);
    return send_all(sock, line + "\n") && recv_line(sock, reply);
}

}  // namespace redon
