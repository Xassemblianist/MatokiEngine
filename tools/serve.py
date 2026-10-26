#!/usr/bin/env python3
"""tools/serve.py — MATOKI geliştirme sunucusu.
web/ dizinini port 8080 üzerinden COOP/COEP ve no-store başlıklarıyla sunar.
"""
import http.server
import os
import socketserver

PORT = 8080
WEB_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")


class MatokiHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "application/javascript",
        ".mjs": "application/javascript",
    }

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    with ReusableTCPServer(("", PORT), MatokiHandler) as httpd:
        print(f"MATOKI sunucusu: http://localhost:{PORT} (kök: {WEB_DIR})")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
