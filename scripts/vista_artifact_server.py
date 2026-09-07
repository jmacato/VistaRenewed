#!/usr/bin/env python3
"""Serve Vista test artifacts to the QEMU user-network guest.

The Windows 11 build VM writes into this workspace through its Y: mapping.
Serving the workspace's test-artifacts directory therefore makes a freshly
built package immediately available to Vista at http://10.0.2.2:<port>/.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "test-artifacts",
        help="directory to expose (default: %(default)s)",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        raise SystemExit(f"artifact directory does not exist: {root}")

    class VistaArtifactHandler(SimpleHTTPRequestHandler):
        def __init__(self, *handler_args, **handler_kwargs):
            super().__init__(
                *handler_args, directory=str(root), **handler_kwargs
            )

        def do_GET(self) -> None:
            if self.path == "/__vista_install_complete__":
                marker = root / ".vista-install-complete"
                marker.write_text(
                    f"{datetime.now(timezone.utc).isoformat()} "
                    f"client={self.client_address[0]}\n",
                    encoding="ascii",
                )
                body = b"VISTA_INSTALL_COMPLETE\r\n"
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            super().do_GET()

        def do_POST(self) -> None:
            result_names = {
                "/__triton9_probe__": ".triton9-probe-result",
                "/__triton9_service__": ".triton9-service-result",
            }
            if self.path not in result_names:
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self.send_error(400, "invalid Content-Length")
                return
            if length <= 0 or length > 1024 * 1024:
                self.send_error(413, "invalid probe result size")
                return
            body = self.rfile.read(length)
            if len(body) != length:
                self.send_error(400, "short probe result")
                return
            (root / result_names[self.path]).write_bytes(body)
            response = b"TRITON9_RESULT_STORED\r\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

    server = ThreadingHTTPServer(("0.0.0.0", args.port), VistaArtifactHandler)
    print(f"Vista artifact server: http://10.0.2.2:{args.port}/ ({root})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
