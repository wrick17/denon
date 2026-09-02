#!/usr/bin/env python3
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, max-age=0")
        super().end_headers()

    def log_message(self, format, *args):
        pass


root = Path(__file__).resolve().parent
os.chdir(root)
server = ThreadingHTTPServer(("127.0.0.1", 8765), Handler)
print("Denon project status: http://127.0.0.1:8765", flush=True)
server.serve_forever()
