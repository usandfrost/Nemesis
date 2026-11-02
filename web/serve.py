#!/usr/bin/env python3
"""Serves the Nemesis website (web/www) on http://localhost:8765 with the headers a shared audio ring buffer needs (cross-origin isolation).   python3 web/serve.py [port]"""
import http.server, json, os, sys, tempfile, threading, time
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "www")
# The listening verdicts (approve / reject + comment per preset) are kept in web/feedback/verdicts.json, which Claude reads (python3 web/tools/verdicts.py).
FB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "feedback", "verdicts.json")
LOCK = threading.Lock()
def load_verdicts():
    try:
        with open(FB) as f: return json.load(f)
    except FileNotFoundError: return {}
    except (OSError, ValueError):   # unreadable or half written: keep it aside rather than overwrite it with one entry
        os.replace(FB, FB + ".bad-%d" % int(time.time()))
        return {}
def save_verdicts(d):
    os.makedirs(os.path.dirname(FB), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(FB), suffix=".tmp")
    with os.fdopen(fd, "w") as f: json.dump(d, f, indent=1, sort_keys=True)
    os.replace(tmp, FB)
class H(http.server.SimpleHTTPRequestHandler):
    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path.split("?")[0] == "/api/verdicts":
            with LOCK: return self.reply(200, load_verdicts())
        super().do_GET()
    def do_POST(self):
        if self.path.split("?")[0] != "/api/verdict": return self.reply(404, {"error": "no such endpoint"})
        # only this page may write: a JSON body (a cross-origin page cannot send one without a preflight, which this server never answers) from our own origin and host
        host = (self.headers.get("Host") or "").lower()
        origin = self.headers.get("Origin")
        hostname = host.split(":")[0]
        mine = hostname in ("localhost", "127.0.0.1") or hostname.endswith(".ts.net")   # this machine, or its name on the user's own tailnet (tailscale serve)
        same = origin is None or origin.split("://", 1)[-1].lower() == host
        if not mine or not same or not (self.headers.get("Content-Type") or "").startswith("application/json"):
            return self.reply(403, {"error": "not from this page"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > 65536: raise ValueError("size")
            v = json.loads(self.rfile.read(n))
            pid = str(int(v["id"])); verdict = v.get("verdict", "")
            if verdict not in ("approved", "rejected", "none"): raise ValueError("verdict")
            rec = {"id": int(pid), "name": str(v.get("name", ""))[:80], "verdict": verdict, "reasons": [str(r)[:40] for r in list(v.get("reasons", []))[:12]],
                   "comment": str(v.get("comment", ""))[:4000], "engine": str(v.get("engine", ""))[:8], "mode": str(v.get("mode", ""))[:12], "register": int(v.get("register", 0)),
                   "grade": str(v.get("grade", ""))[:3], "time": time.strftime("%Y-%m-%d %H:%M:%S"), "handled": False}
        except (ValueError, KeyError, TypeError, OverflowError, AttributeError) as e:
            return self.reply(400, {"error": "bad request: %s" % e})
        with LOCK:
            d = load_verdicts()
            if verdict == "none" and not rec["comment"].strip(): d.pop(pid, None)
            else: d[pid] = rec
            save_verdicts(d)
        self.reply(200, {"ok": True, "time": d.get(pid, {}).get("time", "")})
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm", ".js": "text/javascript", ".json": "application/json", ".wtp": "application/octet-stream", ".woff": "font/woff"}
    def __init__(self, *a, **k): super().__init__(*a, directory=ROOT, **k)
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()
    def log_message(self, *a): pass
if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print("Nemesis website: http://localhost:%d" % port)
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
