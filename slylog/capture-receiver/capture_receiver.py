#!/usr/bin/env python3
"""capture_receiver.py — SlyTherm #181 audit-capture receiver.

The camera Remote POSTs a JPEG (+ metadata in the query string) here whenever
a person makes a manual change at the panel. Stdlib only — runs in a bare
python:3.12-alpine container (see capture-receiver.compose.yaml).

  POST /capture?id=dc25b0&fw=1.3.0&kind=setpoints&detail=...&n=3&photo=1
       body: image/jpeg (or empty for a metadata-only event)
  GET  /            review page: recent events, newest first, with photos
  GET  /photos/...  saved JPEGs

Layout under CAPTURE_DIR (default /captures):
  events.jsonl                    append-only event index (one JSON per line)
  YYYY-MM-DD/HHMMSS_<id>_<kind>.jpg
"""

import html
import json
import os
import re
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

CAPTURE_DIR = os.environ.get("CAPTURE_DIR", "/captures")
PORT = int(os.environ.get("PORT", "8093"))
MAX_BODY = 2 * 1024 * 1024  # a half-res q60 frame is ~60 KB; 2 MB is generous
SAFE = re.compile(r"[^A-Za-z0-9._-]")


def clean(s: str, n: int = 48) -> str:
    return SAFE.sub("_", s)[:n] or "unknown"


class Handler(BaseHTTPRequestHandler):
    server_version = "SlyThermCapture/1"

    def _send(self, code: int, body: bytes, ctype: str = "text/plain") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- ingest ----
    def do_POST(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        if url.path != "/capture":
            self._send(404, b"not found")
            return
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length > MAX_BODY:
            self._send(413, b"too large")
            return
        body = self.rfile.read(length) if length else b""

        now = datetime.now()
        rel = None
        if body:
            if not body.startswith(b"\xff\xd8"):
                self._send(400, b"not a jpeg")
                return
            day = now.strftime("%Y-%m-%d")
            name = "%s_%s_%s.jpg" % (now.strftime("%H%M%S"),
                                     clean(q.get("id", ""), 12),
                                     clean(q.get("kind", ""), 12))
            os.makedirs(os.path.join(CAPTURE_DIR, day), exist_ok=True)
            rel = "%s/%s" % (day, name)
            with open(os.path.join(CAPTURE_DIR, rel), "wb") as f:
                f.write(body)

        event = {
            "ts": now.strftime("%Y-%m-%d %H:%M:%S"),
            "epoch": int(time.time()),
            "id": clean(q.get("id", ""), 12),
            "fw": clean(q.get("fw", ""), 24),
            "kind": clean(q.get("kind", ""), 12),
            "detail": q.get("detail", "")[:96],
            "n": int(q.get("n", "1") or "1"),
            "photo": rel,
        }
        with open(os.path.join(CAPTURE_DIR, "events.jsonl"), "a") as f:
            f.write(json.dumps(event) + "\n")
        self._send(200, b"ok")

    # ---- review ----
    def do_GET(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        if url.path.startswith("/photos/"):
            rel = os.path.normpath(url.path[len("/photos/"):])
            full = os.path.join(CAPTURE_DIR, rel)
            if (rel.startswith("..") or not full.endswith(".jpg")
                    or not os.path.isfile(full)):
                self._send(404, b"not found")
                return
            with open(full, "rb") as f:
                self._send(200, f.read(), "image/jpeg")
            return
        if url.path != "/":
            self._send(404, b"not found")
            return

        events = []
        idx = os.path.join(CAPTURE_DIR, "events.jsonl")
        if os.path.isfile(idx):
            with open(idx) as f:
                for line in f:
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
        events = events[-200:][::-1]  # newest first

        rows = []
        for e in events:
            photo = ('<a href="/photos/%s"><img src="/photos/%s" '
                     'style="height:120px;border-radius:6px"></a>'
                     % (e["photo"], e["photo"])) if e.get("photo") else "&mdash;"
            burst = " &times;%d" % e["n"] if e.get("n", 1) > 1 else ""
            rows.append(
                "<tr><td>%s</td><td>%s</td><td><b>%s</b>%s</td><td>%s</td><td>%s</td></tr>"
                % (html.escape(e.get("ts", "")), html.escape(e.get("id", "")),
                   html.escape(e.get("kind", "")), burst,
                   html.escape(e.get("detail", "")), photo))
        page = ("<!doctype html><title>SlyTherm audit captures</title>"
                "<style>body{font-family:sans-serif;background:#14161a;color:#dde}"
                "table{border-collapse:collapse}td{padding:6px 14px;"
                "border-bottom:1px solid #333;vertical-align:middle}</style>"
                "<h1>SlyTherm audit captures</h1>"
                "<p>%d event(s), newest first (last 200 shown). #181.</p>"
                "<table><tr><td><b>when</b></td><td><b>remote</b></td>"
                "<td><b>change</b></td><td><b>detail</b></td><td><b>photo</b></td></tr>"
                "%s</table>" % (len(events), "".join(rows)))
        self._send(200, page.encode(), "text/html; charset=utf-8")

    def log_message(self, fmt, *args):  # quiet healthcheck noise, keep POSTs
        if self.command == "POST":
            super().log_message(fmt, *args)


if __name__ == "__main__":
    os.makedirs(CAPTURE_DIR, exist_ok=True)
    print("capture-receiver on :%d, saving to %s" % (PORT, CAPTURE_DIR), flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
