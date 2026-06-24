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
  :root{--bg:#0a0c10;--panel:#11151c;--panel2:#161c26;--border:#222a36;--fg:#e8eef6;
        --muted:#8b95a7;--soft:#5b6473;--red:#ff4438;--red2:#c9302c;--ok:#3fb950;
        --amber:#e3b341;--blue:#58a6ff;--radius:13px}
  *{box-sizing:border-box}
  html,body{height:100%}
  body{margin:0;color:var(--fg);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
    background:radial-gradient(1100px 560px at 82% -12%,rgba(255,68,56,.10),transparent 60%),var(--bg)}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
  .app{display:flex;min-height:100vh}
  /* sidebar */
  .sidebar{width:252px;flex:none;border-right:1px solid var(--border);display:flex;
    flex-direction:column;gap:18px;padding:18px 16px;background:linear-gradient(180deg,#0c1017,#0a0c10)}
  .brand{display:flex;align-items:center;gap:12px}
  .brand .mark{width:42px;height:42px;border-radius:12px;display:grid;place-items:center;
    font-weight:800;font-size:21px;color:#fff;background:linear-gradient(135deg,var(--red),var(--red2));
    box-shadow:0 8px 20px rgba(255,68,56,.35)}
  .brand .name{font-size:19px;font-weight:800;letter-spacing:.3px}
  .brand .name span{color:var(--red)}
  .brand .sub{font-size:11px;color:var(--soft);margin-top:2px}
  .conn{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:11px 12px}
  .conn .row{display:flex;align-items:center;gap:9px;font-size:12px}
  .conn .addr{color:var(--soft);margin-top:7px;font-size:11px}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--red);box-shadow:0 0 0 3px rgba(255,68,56,.16)}
  .dot.up{background:var(--ok);box-shadow:0 0 0 3px rgba(63,185,80,.16)}
  .nav{display:flex;flex-direction:column;gap:4px}
  .nav .item{padding:9px 12px;border-radius:9px;color:var(--muted);font-size:13px;font-weight:600}
  .nav .item.active{background:rgba(255,68,56,.12);color:#fff;border:1px solid rgba(255,68,56,.28)}
  .keys{flex:1;display:flex;flex-direction:column;min-height:0}
  .keys h4{margin:0 0 8px;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--soft)}
  .keylist{overflow:auto;display:flex;flex-direction:column;gap:5px}
  .keyrow{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:7px 10px;cursor:pointer}
  .keyrow:hover{border-color:rgba(255,68,56,.45)}
  .keyrow .k{font-size:12px;font-weight:700;word-break:break-all}
  .keyrow .v{font-size:11px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .keys .empty{color:var(--soft);font-size:12px}
  .sidefoot{font-size:11px;color:var(--soft);border-top:1px solid var(--border);padding-top:12px;line-height:1.5}
  /* content */
  .content{flex:1;min-width:0;display:flex;flex-direction:column;gap:16px;padding:22px 26px}
  .topbar{display:flex;align-items:center;gap:14px}
  .topbar h1{margin:0;font-size:20px;font-weight:700}
  .topbar .meta{color:var(--soft);font-size:12px}
  .pill{margin-left:auto;display:flex;align-items:center;gap:8px;background:var(--panel);
    border:1px solid var(--border);border-radius:999px;padding:7px 13px;font-size:12px;color:var(--muted)}
  .cards{display:grid;grid-template-columns:repeat(6,1fr);gap:12px}
  .card{background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--border);
    border-radius:var(--radius);padding:13px 14px}
  .card .label{font-size:10.5px;color:var(--soft);letter-spacing:.05em;text-transform:uppercase}
  .card .val{font-size:25px;font-weight:800;margin-top:6px;font-variant-numeric:tabular-nums;line-height:1}
  .card .val small{font-size:13px;color:var(--muted);font-weight:600;margin-left:2px}
  .chartcard{background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--border);
    border-radius:var(--radius);padding:13px 16px}
  .chartcard .hd{display:flex;align-items:center;gap:10px;margin-bottom:4px}
  .chartcard .hd b{font-size:13px}.chartcard .hd span{font-size:11px;color:var(--soft)}
  #spark{width:100%;height:64px;display:block}
  /* workbench */
  .wb{flex:1;display:flex;flex-direction:column;min-height:300px;overflow:hidden;
    background:linear-gradient(180deg,var(--panel2),var(--panel));border:1px solid var(--border);border-radius:var(--radius)}
  .wb .hd{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid var(--border)}
  .wb .hd b{font-size:13px}.wb .hd .hint{color:var(--soft);font-size:12px}
  .wb .hd .clear{margin-left:auto;font-size:12px;color:var(--muted);cursor:pointer;
    border:1px solid var(--border);border-radius:7px;padding:5px 11px;background:var(--panel)}
  .wb .hd .clear:hover{color:#fff;border-color:var(--red)}
  #console{flex:1;overflow:auto;padding:14px 16px;display:flex;flex-direction:column;gap:11px}
  .entry .q{font-size:13px}
  .entry .q .prompt{color:var(--red);font-weight:800;margin-right:7px;font-family:ui-monospace,Consolas,monospace}
  .entry .q .ts{float:right;color:var(--soft);font-size:11px}
  .entry .a{margin-top:6px}
  .badge{display:inline-block;border-radius:6px;padding:3px 10px;font-size:12px;font-weight:700;
    font-family:ui-monospace,Consolas,monospace}
  .badge.ok{background:rgba(63,185,80,.16);color:var(--ok)}
  .badge.err{background:rgba(255,68,56,.16);color:var(--red)}
  .badge.num{background:rgba(227,179,65,.16);color:var(--amber)}
  .badge.nil{background:rgba(139,149,167,.16);color:var(--muted)}
  .value{background:#070a0e;border:1px solid var(--border);border-radius:8px;padding:9px 11px;
    font-family:ui-monospace,Consolas,monospace;font-size:13px;white-space:pre-wrap;word-break:break-word;color:#d7e2f0}
  .inforow{display:flex;flex-wrap:wrap;gap:6px}
  .kv{background:#070a0e;border:1px solid var(--border);border-radius:7px;padding:4px 9px;font-size:11px;
    font-family:ui-monospace,Consolas,monospace;color:var(--muted)}
  .kv b{color:#fff;font-weight:700}
  .chips{display:flex;gap:7px;flex-wrap:wrap;padding:0 16px 12px}
  .chip{background:var(--panel);border:1px solid var(--border);color:var(--muted);border-radius:999px;
    padding:6px 12px;font-size:12px;cursor:pointer}
  .chip:hover{border-color:var(--red);color:#fff}
  .cmdbar{display:flex;gap:9px;padding:12px 16px;border-top:1px solid var(--border)}
  .cmdbar .prompt{display:flex;align-items:center;color:var(--red);font-weight:800;
    font-family:ui-monospace,Consolas,monospace}
  #cmd{flex:1;background:#070a0e;border:1px solid var(--border);color:var(--fg);border-radius:9px;
    padding:11px 13px;font-family:ui-monospace,Consolas,monospace;font-size:13px}
  #cmd:focus{outline:none;border-color:var(--red)}
  .run{background:linear-gradient(135deg,var(--red),var(--red2));color:#fff;border:0;border-radius:9px;
    padding:0 22px;font-weight:700;cursor:pointer}
  .run:hover{filter:brightness(1.08)}
  ::-webkit-scrollbar{width:9px;height:9px}
  ::-webkit-scrollbar-thumb{background:#283142;border-radius:9px}
  @media(max-width:860px){
    .app{flex-direction:column}
    .sidebar{width:auto;flex-direction:row;flex-wrap:wrap;align-items:center}
    .keys,.nav{display:none}.cards{grid-template-columns:repeat(3,1fr)}
  }
</style>
</head>
<body>
<div class="app">
  <aside class="sidebar">
    <div class="brand">
      <div class="mark">R</div>
      <div><div class="name">Re<span>don</span></div><div class="sub">distributed KV store · C++</div></div>
    </div>
    <div class="conn">
      <div class="row"><span id="dot" class="dot"></span><span id="connText">connecting…</span></div>
      <div class="addr">browser → redon-web → server</div>
    </div>
    <div class="nav">
      <div class="item active">Workbench</div>
      <div class="item">Live metrics</div>
    </div>
    <div class="keys">
      <h4>Keys · this session</h4>
      <div class="keylist" id="keylist"><div class="empty">No keys yet — run a SET.</div></div>
    </div>
    <div class="sidefoot">A 9-phase distributed key-value store,<br>built from scratch. Redis-inspired.</div>
  </aside>
  <main class="content">
    <div class="topbar">
      <h1>Workbench</h1>
      <span class="meta">browser console for Redon</span>
      <div class="pill"><span id="dot2" class="dot"></span><span id="pillText">connecting…</span></div>
    </div>
    <div class="cards" id="cards"></div>
    <div class="chartcard">
      <div class="hd"><b>Throughput</b><span>operations / sec, live</span>
        <span id="opsNow" style="margin-left:auto;color:var(--fg);font-weight:700"></span></div>
      <canvas id="spark"></canvas>
    </div>
    <div class="wb">
      <div class="hd"><b>CLI</b><span class="hint">— type a command, ↑/↓ for history</span>
        <span class="clear" id="clearBtn">Clear</span></div>
      <div id="console"></div>
      <div class="chips" id="chips"></div>
      <form id="form" class="cmdbar" autocomplete="off">
        <span class="prompt">redon&gt;</span>
        <input id="cmd" class="mono" type="text" placeholder="SET user:1 Saurabh" autofocus>
        <button class="run" type="submit">Run</button>
      </form>
    </div>
  </main>
</div>
)PAGE"
// (split here only because MSVC caps a single string literal at ~16 KB; adjacent
// string literals are concatenated by the compiler into one page.)
R"PAGE(<script>
const $=id=>document.getElementById(id);
const consoleEl=$('console'),keylist=$('keylist'),cardsEl=$('cards');
const dot=$('dot'),dot2=$('dot2'),connText=$('connText'),pillText=$('pillText');
const keys=new Map();
const hist=[];let hpos=-1;

function nowts(){return new Date().toTimeString().slice(0,8);}
function badge(reply){
  if(reply==='OK'||reply==='PONG')return ['ok',reply];
  if(reply.indexOf('ERR')===0)return ['err',reply];
  if(reply==='(nil)')return ['nil','(nil)'];
  if(reply.indexOf('(integer)')===0)return ['num',reply];
  return [null,reply];
}
function renderInfo(line){
  const wrap=document.createElement('div');wrap.className='inforow';
  line.split(' ').forEach(kv=>{const i=kv.indexOf('=');if(i<0)return;
    const d=document.createElement('span');d.className='kv';
    d.appendChild(document.createTextNode(kv.slice(0,i)+' '));
    const b=document.createElement('b');b.textContent=kv.slice(i+1);d.appendChild(b);
    wrap.appendChild(d);});
  return wrap;
}
function addEntry(cmd,reply,ok){
  const e=document.createElement('div');e.className='entry';
  const q=document.createElement('div');q.className='q';
  const p=document.createElement('span');p.className='prompt';p.textContent='redon>';
  const c=document.createElement('span');c.textContent=cmd;
  const ts=document.createElement('span');ts.className='ts';ts.textContent=nowts();
  q.appendChild(p);q.appendChild(c);q.appendChild(ts);
  const a=document.createElement('div');a.className='a';
  if(!ok){const b=document.createElement('span');b.className='badge err';b.textContent='✗ '+reply;a.appendChild(b);}
  else if(reply.indexOf('uptime_s=')===0){a.appendChild(renderInfo(reply));}
  else{const r=badge(reply);
    if(r[0]){const b=document.createElement('span');b.className='badge '+r[0];b.textContent=r[1];a.appendChild(b);}
    else{const v=document.createElement('div');v.className='value';v.textContent=r[1];a.appendChild(v);}}
  e.appendChild(q);e.appendChild(a);consoleEl.appendChild(e);
  consoleEl.scrollTop=consoleEl.scrollHeight;
}
function trackKey(cmd,reply){
  const t=cmd.trim().split(/\s+/);const verb=(t[0]||'').toUpperCase();const k=t[1];
  if(!k)return;
  if(verb==='SET'&&reply==='OK')keys.set(k,t.slice(2).join(' '));
  else if(verb==='GET'&&reply!=='(nil)'&&reply.indexOf('ERR')!==0)keys.set(k,reply);
  else if((verb==='DEL'||verb==='DELETE')&&reply.indexOf('1')>=0)keys.delete(k);
  renderKeys();
}
function renderKeys(){
  keylist.innerHTML='';
  if(keys.size===0){const d=document.createElement('div');d.className='empty';
    d.textContent='No keys yet — run a SET.';keylist.appendChild(d);return;}
  [...keys.keys()].sort().forEach(k=>{
    const r=document.createElement('div');r.className='keyrow';
    const kk=document.createElement('div');kk.className='k';kk.textContent=k;
    const vv=document.createElement('div');vv.className='v';vv.textContent=keys.get(k);
    r.appendChild(kk);r.appendChild(vv);r.onclick=()=>run('GET '+k);keylist.appendChild(r);});
}
async function api(cmd){const r=await fetch('/cmd',{method:'POST',body:cmd});return r.json();}
async function run(cmd){
  cmd=(cmd||'').trim();if(!cmd)return;
  hist.push(cmd);hpos=hist.length;
  try{const j=await api(cmd);
    if(j.ok){addEntry(cmd,j.reply,true);trackKey(cmd,j.reply);}
    else addEntry(cmd,j.error||'error',false);
  }catch(e){addEntry(cmd,'network error',false);}
  pollStats();
}
$('form').addEventListener('submit',e=>{e.preventDefault();const i=$('cmd');run(i.value);i.value='';});
$('cmd').addEventListener('keydown',e=>{
  if(e.key==='ArrowUp'){if(hpos>0){hpos--;$('cmd').value=hist[hpos];e.preventDefault();}}
  else if(e.key==='ArrowDown'){if(hpos<hist.length-1){hpos++;$('cmd').value=hist[hpos];}
    else{hpos=hist.length;$('cmd').value='';}}
});
$('clearBtn').onclick=()=>{consoleEl.innerHTML='';};
const chips=[['PING','PING'],['SET','SET user:1 Saurabh'],['GET','GET user:1'],
  ['EXISTS','EXISTS user:1'],['DEL','DEL user:1'],['INFO','INFO']];
const chipBox=$('chips');
chips.forEach(c=>{const b=document.createElement('span');b.className='chip';
  b.textContent=c[0];b.title=c[1];b.onclick=()=>run(c[1]);chipBox.appendChild(b);});

const CARDS=[['keys','Keys'],['commands','Commands'],['ops','Ops/sec'],
  ['hit_rate','Hit rate'],['clients','Clients'],['uptime_s','Uptime']];
function fmtUptime(s){s=+s;if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m';
  return Math.floor(s/3600)+'h';}
function renderCards(p,ops){
  cardsEl.innerHTML='';
  CARDS.forEach(c=>{
    let v='—',small='';
    if(c[0]==='ops'){if(ops!=null)v=Math.round(ops).toString();}
    else if(c[0]==='hit_rate'){if(p.hit_rate!=null){v=(parseFloat(p.hit_rate)*100).toFixed(0);small='%';}}
    else if(c[0]==='uptime_s'){if(p.uptime_s!=null)v=fmtUptime(p.uptime_s);}
    else{if(p[c[0]]!=null)v=p[c[0]];}
    const d=document.createElement('div');d.className='card';
    const lab=document.createElement('div');lab.className='label';lab.textContent=c[1];
    const val=document.createElement('div');val.className='val';val.textContent=v;
    if(small){const sm=document.createElement('small');sm.textContent=small;val.appendChild(sm);}
    d.appendChild(lab);d.appendChild(val);cardsEl.appendChild(d);
  });
}
const spark=$('spark'),sctx=spark.getContext('2d'),samples=[],MAX=80;
function drawSpark(){
  const dpr=window.devicePixelRatio||1,w=spark.clientWidth,h=spark.clientHeight;
  spark.width=w*dpr;spark.height=h*dpr;sctx.setTransform(dpr,0,0,dpr,0,0);sctx.clearRect(0,0,w,h);
  if(samples.length<2)return;
  const mx=Math.max(1,...samples),n=samples.length;
  const X=i=>i/(MAX-1)*w,Y=v=>h-4-(v/mx)*(h-10);
  const g=sctx.createLinearGradient(0,0,0,h);
  g.addColorStop(0,'rgba(255,68,56,.35)');g.addColorStop(1,'rgba(255,68,56,0)');
  sctx.beginPath();sctx.moveTo(X(0),Y(samples[0]));
  for(let i=1;i<n;i++)sctx.lineTo(X(i),Y(samples[i]));
  sctx.lineTo(X(n-1),h);sctx.lineTo(X(0),h);sctx.closePath();sctx.fillStyle=g;sctx.fill();
  sctx.beginPath();sctx.moveTo(X(0),Y(samples[0]));
  for(let i=1;i<n;i++)sctx.lineTo(X(i),Y(samples[i]));
  sctx.strokeStyle='#ff4438';sctx.lineWidth=2;sctx.stroke();
}
window.addEventListener('resize',drawSpark);

let lastCmds=null,lastT=null;
function setConn(up,text){dot.className='dot'+(up?' up':'');dot2.className='dot'+(up?' up':'');
  connText.textContent=text;pillText.textContent=text;}
async function pollStats(){
  try{const j=await api('INFO');
    if(j.ok&&j.reply.indexOf('uptime_s=')>=0){
      const p={};j.reply.split(' ').forEach(kv=>{const i=kv.indexOf('=');if(i>0)p[kv.slice(0,i)]=kv.slice(i+1);});
      const t=Date.now();let ops=null;
      if(lastCmds!=null&&lastT!=null){const dt=(t-lastT)/1000;if(dt>0)ops=Math.max(0,(+p.commands-lastCmds)/dt);}
      lastCmds=+p.commands;lastT=t;
      if(ops!=null){samples.push(ops);if(samples.length>MAX)samples.shift();drawSpark();
        $('opsNow').textContent=Math.round(ops)+' ops/s';}
      renderCards(p,ops);setConn(true,'connected to Redon');
    }else setConn(false,'Redon unreachable');
  }catch(e){setConn(false,'gateway down');}
}
const intro=document.createElement('div');intro.className='entry';
const iv=document.createElement('div');iv.className='value';
iv.textContent='Welcome to Redon. Type a command or click a chip below. Try: SET user:1 Saurabh';
intro.appendChild(iv);consoleEl.appendChild(intro);
renderCards({},null);drawSpark();pollStats();setInterval(pollStats,1500);
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
