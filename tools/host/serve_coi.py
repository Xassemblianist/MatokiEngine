#!/usr/bin/env python3
"""tools/host/serve_coi.py — Başsız WebGPU / SharedArrayBuffer testleri için COOP/COEP HTTP sunucusu.
Kullanım: python3 tools/host/serve_coi.py [port] [dizin]
Varsayılan port: 8095, varsayılan dizin: web/ (ayrıca /web/* ve /build/* yollarını da çözer).
"""
import http.server
import os
import socketserver
import sys
import urllib.parse

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_WEB_DIR = os.path.join(REPO_ROOT, "web")


class CoiHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "application/javascript",
        ".mjs": "application/javascript",
        ".wgsl": "text/plain; charset=utf-8",
    }

    def __init__(self, *args, root_dir=DEFAULT_WEB_DIR, **kwargs):
        self._root_dir = os.path.abspath(root_dir)
        super().__init__(*args, directory=self._root_dir, **kwargs)

    def translate_path(self, path):
        parsed = urllib.parse.urlsplit(path).path
        clean = urllib.parse.unquote(parsed)
        if clean.startswith("/build/"):
            cand = os.path.normpath(os.path.join(REPO_ROOT, clean.lstrip("/")))
            if cand.startswith(os.path.join(REPO_ROOT, "build")):
                return cand
        if clean.startswith("/web/"):
            cand = os.path.normpath(os.path.join(REPO_ROOT, clean.lstrip("/")))
            if cand.startswith(DEFAULT_WEB_DIR):
                return cand
        return super().translate_path(path)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format, *args):
        if os.environ.get("SERVE_COI_VERBOSE"):
            super().log_message(format, *args)


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8095
    root_dir = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_WEB_DIR

    def handler(*args, **kwargs):
        return CoiHandler(*args, root_dir=root_dir, **kwargs)

    with ReusableTCPServer(("127.0.0.1", port), handler) as httpd:
        print(f"COI sunucusu hazır: http://127.0.0.1:{port} (kök: {root_dir})", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
