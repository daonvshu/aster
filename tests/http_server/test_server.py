import concurrent.futures
import http.client
from pathlib import Path
import tempfile
import threading
import time
import unittest

from run_tests import png
from server import ImageServer


class ServerTests(unittest.TestCase):
    def test_routes_and_concurrency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            png(root / "avatar.png", (255, 0, 0))
            with ImageServer(("127.0.0.1", 0), root) as server:
                worker = threading.Thread(target=server.serve_forever, daemon=True)
                worker.start()

                def get(path, headers=None):
                    connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
                    try:
                        connection.request("GET", path, headers=headers or {})
                        response = connection.getresponse()
                        return response.status, dict(response.getheaders()), response.read()
                    finally:
                        connection.close()

                try:
                    with concurrent.futures.ThreadPoolExecutor(2) as pool:
                        slow = pool.submit(get, "/delay/1/avatar.png")
                        time.sleep(0.1)
                        normal = get("/image/avatar.png")
                        self.assertFalse(slow.done())
                        self.assertEqual(slow.result()[2], normal[2])
                    etag = get("/etag/avatar.png")
                    validated = get("/etag/avatar.png", {"If-None-Match": etag[1]["ETag"]})
                    self.assertEqual(validated[0], 304)
                    self.assertEqual(validated[2], b"")
                    self.assertEqual(get("/cache/avatar.png")[1]["Cache-Control"], "public, max-age=3600")
                    self.assertEqual(get("/redirect/avatar.png")[0], 302)
                    self.assertEqual(get("/wrong-content-type/avatar.png")[2], normal[2])
                    self.assertNotEqual(get("/corrupt/avatar.png")[2], normal[2])
                    self.assertEqual(get("/stream/avatar.png")[2], normal[2])
                    with self.assertRaises(http.client.IncompleteRead):
                        get("/truncate/avatar.png")
                    self.assertEqual(get("/image/%2e%2e/server.py")[0], 404)
                    self.assertEqual(get("/delay/")[0], 400)
                    self.assertEqual(get("/status/500")[0], 500)
                finally:
                    server.shutdown()
                    worker.join()


if __name__ == "__main__":
    unittest.main()
