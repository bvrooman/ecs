#!/usr/bin/env python3
"""Static file server that sets the cross-origin-isolation headers (COOP/COEP).

SharedArrayBuffer -- and therefore the -pthread multi-threaded build
(build-wasm/web/index-mt.html) -- is only available on a cross-origin-isolated
page. The stock `python -m http.server` does not send these headers, so use this
instead. The single-threaded page is unaffected by the headers, so this serves
both. Usage: coi-server.py [port] [directory]
"""
import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else "."
    handler = functools.partial(Handler, directory=directory)
    print(f"serving {directory} at http://localhost:{port}/  (cross-origin isolated)")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()
