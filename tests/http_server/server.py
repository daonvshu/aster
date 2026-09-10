"""Zero-dependency, concurrent HTTP image fault server (loopback by default)."""

import argparse
import hashlib
import json
import mimetypes
from pathlib import Path
import socket
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit


class ImageServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, directory):
        self.directory = Path(directory).resolve(strict=True)
        if not self.directory.is_dir():
            raise ValueError("Image directory must be a directory")
        super().__init__(address, ImageHandler)


class ImageHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def respond(self, status, body=b"", headers=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.close_connection = True
        if body:
            self.wfile.write(body)

    def do_GET(self):
        try:
            self.handle_image()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # Expected when testing cancellation and response limits.
        except (ValueError, OverflowError, IndexError):
            self.respond(400, b"Invalid request")
        except OSError:
            self.close_connection = True

    def handle_image(self):
        parts = unquote(urlsplit(self.path).path).strip("/").split("/")
        route = parts.pop(0)
        if route == "health":
            self.respond(200, b"ok")
            return
        if route == "status":
            code = int(parts[0]) if len(parts) == 1 else 400
            self.respond(code if code in (404, 500) else 400)
            return
        delay = 0.0
        if route == "delay":
            delay = float(parts.pop(0))
            if not 0 <= delay <= 30:
                raise ValueError("Delay out of range")
        if route not in {"image", "delay", "redirect", "etag", "cache", "no-store",
                         "corrupt", "truncate", "wrong-content-type", "oversize", "stream"}:
            self.respond(404)
            return
        path = (self.server.directory / "/".join(parts)).resolve()
        if not path.is_relative_to(self.server.directory) or not path.is_file():
            self.respond(404)
            return
        if route == "redirect":
            self.respond(302, headers={"Location": "/image/" + quote("/".join(parts))})
            return
        body = path.read_bytes()
        headers = {"Content-Type": mimetypes.guess_type(path.name)[0] or "application/octet-stream",
                   "Cache-Control": "no-store"}
        if route == "etag":
            etag = '"' + hashlib.sha256(body).hexdigest() + '"'
            headers.update({"ETag": etag, "Cache-Control": "no-cache"})
            if self.headers.get("If-None-Match") == etag:
                self.respond(304, headers=headers)
                return
        elif route == "cache":
            headers["Cache-Control"] = "public, max-age=3600"
        elif route == "wrong-content-type":
            headers["Content-Type"] = "text/plain"
        elif route == "corrupt":
            body = b"This is not an image."
        time.sleep(delay)
        if route in {"truncate", "stream", "oversize"}:
            length = 40 * 1024 * 1024 if route == "oversize" else len(body)
            self.send_response(200)
            for name, value in headers.items():
                self.send_header(name, value)
            self.send_header("Content-Length", str(length))
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            if route == "truncate":
                self.wfile.write(body[:max(1, len(body) // 2)])
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_WR)
                return
            chunk = b"x" * 65536 if route == "oversize" else body
            step = 65536 if route == "oversize" else max(1, len(body) // 20)
            for offset in range(0, length, step):
                self.wfile.write(chunk[:min(step, length - offset)] if route == "oversize"
                                 else body[offset:offset + step])
                self.wfile.flush()
                if route == "stream":
                    time.sleep(0.05)
            return
        self.respond(200, body, headers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=0, type=int)
    args = parser.parse_args()
    with ImageServer((args.host, args.port), args.images) as server:
        print(json.dumps({"port": server.server_port,
                          "url": f"http://{args.host}:{server.server_port}"}), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
