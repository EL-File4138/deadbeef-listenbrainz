# SPDX-License-Identifier: Unlicense
import contextlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Server(ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.calls = queue.Queue()
        self.responses = queue.Queue()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.respond(
            None,
            {
                "valid": self.headers["Authorization"] != "Token invalid",
                "user_name": "test-account",
            },
        )

    def do_POST(self):
        self.respond(
            json.loads(self.rfile.read(int(self.headers["Content-Length"]))),
            {"status": "ok"},
        )

    def respond(self, payload, default):
        try:
            status, body, headers = self.server.responses.get_nowait()
        except queue.Empty:
            status, body, headers = 200, default, {}
        self.server.calls.put(
            (self.path, self.headers["Authorization"], payload, time.monotonic())
        )
        self.send_response(status)
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def log_message(self, *_):
        pass


class Tests(unittest.TestCase):
    def setUp(self):
        self.directory = self.enterContext(tempfile.TemporaryDirectory())
        self.server = Server()
        self.addCleanup(self.server.server_close)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.shutdown)
        self.base = f"http://127.0.0.1:{self.server.server_port}/proxy/listenbrainz"
        self.file = Path(self.directory, "listenbrainz", "queue.json")

    @contextlib.contextmanager
    def host(self, token="secret-one", mode="", base=None):
        with subprocess.Popen(
            ["build/test-host", base or self.base, token, self.directory, mode],
            stdin=subprocess.PIPE,
            text=True,
            env={**os.environ, "NO_PROXY": "127.0.0.1"},
        ) as p:

            def send(events):
                p.stdin.write(events)
                p.stdin.flush()

            try:
                yield send
            finally:
                send("q\n")
                self.assertEqual(p.wait(timeout=12), 0)

    def receive(self, endpoint):
        path, token, data, at = self.server.calls.get(timeout=12)
        self.assertTrue(path.endswith("/1/" + endpoint), path)
        return token, data, at

    def test_payloads_and_stop(self):
        with self.host(mode="n", base=self.base + "/1/") as send:
            send("n\n")
            self.receive("validate-token")
            _, now, _ = self.receive("submit-listens")
            self.assertEqual(now["listen_type"], "playing_now")
            self.assertNotIn("listened_at", now["payload"][0])
            send("s\nf\nc\n")
            _, clear, _ = self.receive("playing-now/delete")
            self.assertEqual(clear["client"], "DeaDBeeF ListenBrainz Plugin")
            auth, data, _ = self.receive("submit-listens")
            self.assertEqual(auth, "Token secret-one")
            self.assertEqual(data["listen_type"], "single")
            (listen,) = data["payload"]
            self.assertEqual(listen["listened_at"], 1700000001)
            self.assertEqual(listen["track_metadata"]["artist_name"], "Björk")
            self.assertEqual(listen["track_metadata"]["track_name"], 'Test "title"')
            self.assertEqual(
                listen["track_metadata"]["additional_info"]["duration_ms"], 10000
            )

    def test_invalid_token_and_rotation(self):
        with self.host(token="invalid") as send:
            send("f\n")
            self.receive("validate-token")
            with self.assertRaises(queue.Empty):
                self.server.calls.get(timeout=1.2)
        self.assertNotIn("invalid", self.file.read_text())
        self.server.responses.put(
            (200, {"valid": True, "user_name": "test-account"}, {})
        )
        self.server.responses.put((503, {}, {}))
        with self.host() as send:
            send("f\n")
            self.receive("validate-token")
            self.receive("submit-listens")
        with self.host(token="secret-two", base=self.base + "/other") as send:
            self.receive("validate-token")
            send("o3\nx1\n")
            with self.assertRaises(queue.Empty):
                self.server.calls.get(timeout=1.2)
        with self.host(token="secret-two") as send:
            self.receive("validate-token")
            with self.assertRaises(queue.Empty):
                self.server.calls.get(timeout=1.2)
            send("o3\nx1\n")
            auth, data, _ = self.receive("submit-listens")
            self.assertEqual(auth, "Token secret-two")
            self.assertEqual(data["payload"][0]["listened_at"], 1700000000)

    def test_discard_requires_new_run(self):
        with self.host(token="invalid") as send:
            send("f\n")
        queued = self.file.read_text()
        self.assertEqual(len(json.loads(queued)), 1)

        with self.host() as send:
            send("o4\n")
        self.assertEqual(self.file.read_text(), queued)

        with self.host(mode="d") as send:
            send("x1\no1\no4\n")
        self.assertEqual(self.file.read_text(), queued)

        with self.host(mode="d") as send:
            send("x0\nx1\n")
        self.assertEqual(json.loads(self.file.read_text()), [])

    def test_rate_limit_and_unexpected_success_body(self):
        self.server.responses.put((404, {}, {}))
        self.server.responses.put((429, {}, {"X-RateLimit-Reset-In": "3"}))
        self.server.responses.put((200, {"error": "wrong route"}, {}))
        with self.host() as send:
            self.receive("validate-token")
            send("f\n")
            _, first, then = self.receive("submit-listens")
            _, second, now = self.receive("submit-listens")
            self.assertEqual(first, second)
            self.assertGreaterEqual(now - then, 2.9)
        self.assertEqual(len(json.loads(self.file.read_text())), 1)

    def test_rejected_batch_falls_back_to_singles(self):
        self.server.responses.put(
            (200, {"valid": True, "user_name": "test-account"}, {})
        )
        self.server.responses.put((400, {"error": "import not supported"}, {}))
        with self.host(mode="b") as send:
            send("f\nf\n")
            self.receive("validate-token")
            _, batch, _ = self.receive("submit-listens")
            self.assertEqual(batch["listen_type"], "import")
            for listen in batch["payload"]:
                _, single, _ = self.receive("submit-listens")
                self.assertEqual(single, {"listen_type": "single", "payload": [listen]})
            self.assertEqual(len(batch["payload"]), 2)


if __name__ == "__main__":
    unittest.main()
