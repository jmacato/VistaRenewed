#!/usr/bin/env python3
"""Controlled HTTP document, bound only inside the QEMU container loopback."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading

BODY = b'''<!doctype html><html><head><title>HTTP control document</title></head>
<body style="background:#102b39;color:#eef7fa;font:24px Arial;padding:32px">
<h1>HTTP control document</h1><p>Native IE network handoff test</p>
<p id="engine"></p><script>
document.getElementById('engine').appendChild(document.createTextNode(navigator.userAgent));
</script></body></html>'''


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/engine-probe':
            self.send_error(404)
            return
        print('HTTP_CONTROL_GET agent=' + self.headers.get('User-Agent', '')[:256], flush=True)
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(BODY)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18765)
    parser.add_argument('--duration', type=int, default=180)
    args = parser.parse_args()
    if not 1 <= args.duration <= 900:
        parser.error('duration must be 1..900 seconds')
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    timer = threading.Timer(args.duration, server.shutdown)
    timer.start()
    print(f'HTTP_CONTROL_READY port={server.server_port}', flush=True)
    try:
        server.serve_forever()
    finally:
        timer.cancel()
        server.server_close()


if __name__ == '__main__':
    main()
