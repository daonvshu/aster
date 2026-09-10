"""Start an isolated image server, run a test command, always stop the server."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import zlib

from server import ImageServer


def png(path, color):
    def chunk(kind, payload):
        return struct.pack("!I", len(payload)) + kind + payload + struct.pack(
            "!I", zlib.crc32(kind + payload))
    pixels = (b"\0" + bytes(color) * 16) * 16
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack("!2I5B", 16, 16, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path)
    parser.add_argument("--timeout", type=int, default=100)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("A test command is required")
    with tempfile.TemporaryDirectory(prefix="aster-http-") as temporary:
        directory = args.images or Path(temporary)
        if not args.images:
            png(directory / "avatar.png", (255, 0, 0))
            png(directory / "landscape.png", (0, 0, 255))
        with ImageServer(("127.0.0.1", 0), directory) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            env = dict(os.environ, ASTER_IMAGE_TEST_URL=f"http://127.0.0.1:{server.server_port}")
            print("Image Test Server: " + env["ASTER_IMAGE_TEST_URL"], flush=True)
            try:
                result = subprocess.run(command, env=env, timeout=args.timeout).returncode
                if "-o" in command:
                    output = Path(command[command.index("-o") + 1].rsplit(",", 1)[0])
                    if output.is_file():
                        print(output.read_text(encoding="utf-8", errors="replace"), flush=True)
                return result
            except subprocess.TimeoutExpired:
                print("HTTP integration test timed out", flush=True)
                return 124
            finally:
                server.shutdown()
                worker.join()


if __name__ == "__main__":
    raise SystemExit(main())
