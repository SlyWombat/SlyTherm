"""Capture-session annotation logger (#27 aid).

A dead-simple, phone-friendly page for logging what the OPERATOR is doing at
the OEM thermostat during a bus-capture session ("fan hi", "cool 14", ...).
Each entry is server-timestamped with postgres now() — the SAME clock as
raw_frames.ts — so the log can be correlated against the CT-485 wire exactly,
with no LLM in the loop and no phone-clock skew. Entries land in
events(kind='annotation', source='capture-logger').

No auth (LAN-only, same trust boundary as Grafana). Stdlib http.server +
psycopg3 (PG* env, like the collector). Port 8091.
"""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import psycopg

TZ = os.environ.get("TZ", "America/Toronto")

PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>SlyTherm capture log</title>
<style>
 :root{color-scheme:dark}
 body{font-family:system-ui,sans-serif;margin:0;background:#0e1116;color:#e6edf3;
      -webkit-text-size-adjust:100%}
 header{padding:12px 14px;background:#161b22;font-weight:600;font-size:18px;
        border-bottom:1px solid #30363d}
 .wrap{padding:12px 14px 90px}
 input[type=text]{width:100%;box-sizing:border-box;font-size:20px;padding:14px;
   border-radius:10px;border:1px solid #30363d;background:#0d1117;color:#e6edf3}
 .row{display:flex;gap:8px;margin-top:8px}
 button{flex:1;font-size:17px;padding:14px 8px;border-radius:10px;border:0;
   background:#21262d;color:#e6edf3;font-weight:600}
 button:active{background:#30363d}
 .log{background:#0f60b6}.done{background:#8b1f1f}
 .quick{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}
 .quick button{background:#1b2430;font-size:15px;padding:12px 4px}
 h3{margin:18px 0 6px;color:#8b949e;font-size:13px;text-transform:uppercase;letter-spacing:.5px}
 ul{list-style:none;margin:0;padding:0}
 li{padding:9px 4px;border-bottom:1px solid #21262d;font-size:16px;display:flex;gap:10px}
 li .t{color:#58a6ff;font-variant-numeric:tabular-nums;flex:0 0 auto}
 .ok{position:fixed;bottom:0;left:0;right:0;padding:12px;text-align:center;
   background:#161b22;border-top:1px solid #30363d;font-size:14px;color:#8b949e}
</style></head><body>
<header>SlyTherm capture log</header>
<div class="wrap">
 <input id="t" type="text" autocomplete="off" autocapitalize="none"
   placeholder="e.g. fan hi  /  cool 14  /  mode heat" enterkeyhint="send">
 <div class="row">
   <button class="log" onclick="send(document.getElementById('t').value)">LOG</button>
   <button class="done" onclick="send('DONE')">DONE</button>
 </div>
 <div class="quick" id="q"></div>
 <h3>logged this session</h3>
 <ul id="list"></ul>
</div>
<div class="ok" id="ok">ready — every entry is stamped on the server clock</div>
<script>
const QUICK=['fan low','fan med','fan hi','fan auto','fan on',
 'mode cool','mode heat','mode off','cool 14','cool 21','cool 30','heat'];
const q=document.getElementById('q');
QUICK.forEach(x=>{const b=document.createElement('button');b.textContent=x;
 b.onclick=()=>send(x);q.appendChild(b)});
const inp=document.getElementById('t');
inp.addEventListener('keydown',e=>{if(e.key==='Enter'){send(inp.value)}});
function render(rows){document.getElementById('list').innerHTML=
 rows.map(r=>`<li><span class="t">${r.t}</span><span>${r.note}</span></li>`).join('')}
async function send(text){text=(text||'').trim();if(!text)return;
 document.getElementById('ok').textContent='logging…';
 try{const r=await fetch('/log',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({text})});const j=await r.json();
   if(j.ok){inp.value='';render(j.recent);
     document.getElementById('ok').textContent='logged "'+text+'"  ✓';}
   else{document.getElementById('ok').textContent='ERROR: '+(j.err||'?')}}
 catch(e){document.getElementById('ok').textContent='network error — retry'}}
fetch('/recent').then(r=>r.json()).then(render);
</script></body></html>"""


def _db():
    return psycopg.connect(autocommit=True)  # PGHOST/PGUSER/... from env


def log_entry(text: str) -> None:
    with _db() as c:
        c.execute(
            "INSERT INTO events (ts, kind, detail) VALUES "
            "(now(), 'annotation', jsonb_build_object("
            "'note', %s::text, 'source', 'capture-logger'))",
            (text,),
        )


def recent(n: int = 30):
    with _db() as c:
        rows = c.execute(
            "SELECT to_char(ts AT TIME ZONE %s::text, 'HH24:MI:SS'), detail->>'note' "
            "FROM events WHERE kind='annotation' "
            "AND detail->>'source'='capture-logger' "
            "ORDER BY ts DESC LIMIT %s",
            (TZ, n),
        ).fetchall()
    return [{"t": r[0], "note": r[1]} for r in rows]


class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.startswith("/recent"):
            self._send(200, json.dumps(recent()))
        elif self.path.startswith("/health"):
            self._send(200, json.dumps({"ok": True}))
        else:
            self._send(200, PAGE, "text/html; charset=utf-8")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        try:
            text = json.loads(raw).get("text", "")
        except Exception:
            text = raw
        text = (text or "").strip()[:200]
        if text:
            try:
                log_entry(text)
            except Exception as e:  # never 500 silently — surface it to the phone
                self._send(500, json.dumps({"ok": False, "err": str(e)}))
                return
        self._send(200, json.dumps({"ok": True, "recent": recent()}))

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print("capture annotator on :8091", flush=True)
    ThreadingHTTPServer(("0.0.0.0", 8091), H).serve_forever()
