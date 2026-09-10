#!/usr/bin/env python3
"""Compile and exercise the actual scoped transport with local HTTP/TLS servers.

Run in a disposable Linux container with --network none and a writable /tmp.
No document-server processes, credentials, external network or Python packages needed.
"""
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time
import unittest


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    hits = []

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.hits.append(self.path)
        try:
            if self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/escaped')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if self.path == '/slow':
                time.sleep(2)
            if self.path == '/chunked':
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for _ in range(128):
                    self.wfile.write(b'1000\r\n' + b'x' * 4096 + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
                return
            payload = b'trusted-document-bytes'
            self.send_response(500 if self.path == '/error' else 200)
            self.send_header('Content-Length', str(len(payload) + (1 if self.path == '/truncated' else 0)))
            self.end_headers()
            self.wfile.write(payload)
            if self.path == '/truncated':
                self.close_connection = True
        except (BrokenPipeError, ConnectionResetError):
            pass


class RestrictedDownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='restricted-download-')
        cls.root = Path(cls.temp.name)
        cls.exe = cls.root / 'harness'
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-O2',
                        str(Path(__file__).with_name('restricted_download_harness.cpp')),
                        '-o', str(cls.exe)], check=True)
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.origin = f'http://127.0.0.1:{cls.server.server_port}/'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.temp.cleanup()

    def setUp(self):
        self.output = self.root / 'result'
        self.output.unlink(missing_ok=True)
        Handler.hits.clear()

    def run_download(self, path='ok', allow=None, limit=1024, timeout=3000, cancel=0, env=None):
        result = subprocess.run([str(self.exe), 'download', self.origin + path,
                                 self.origin if allow is None else allow, str(self.output),
                                 str(limit), str(timeout), str(cancel)], env=env, timeout=5)
        self.assertIn(result.returncode, [0, 1], 'transport must reap every child')
        if result.returncode:
            self.assertFalse(self.output.exists(), 'failed download must remove partial output')
        return result.returncode

    def test_exact_origin_success_and_permissions(self):
        self.assertEqual(self.run_download(), 0)
        self.assertEqual(self.output.read_bytes(), b'trusted-document-bytes')
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(Handler.hits, ['/ok'])

    def test_policy_denials_never_connect(self):
        bad = ['', self.origin.rstrip('/'), self.origin + 'prefix/', self.origin + ';',
               self.origin + ';malformed', self.origin.replace('127.0.0.1', '127.0.0.1.evil'),
               self.origin.replace('http:', 'https:'), 'http://127.000.0.1/', 'http://0x7f000001/']
        for origin in bad:
            with self.subTest(origin=origin):
                self.assertEqual(self.run_download(allow=origin), 1)
        self.assertEqual(Handler.hits, [])

    def test_ambiguous_urls_denied(self):
        variants = ['http://127.1/', 'http://2130706433/', 'http://0x7f.0.0.1/',
                    'http://user@127.0.0.1/', 'http://127.0.0.1:080/', 'http://HOST/',
                    'http://host./', 'file:///tmp/x', 'ftp://host/', 'http://host/a/../b',
                    'http://host/%2f', 'http://host/a\\b', 'http://host/a#fragment',
                    'http://host//a', 'http://host/\n', 'http://host/a%252f']
        for url in variants:
            with self.subTest(url=url):
                self.assertEqual(subprocess.run([str(self.exe), 'policy', url, url]).returncode, 1)

    def test_redirect_refused_without_second_request(self):
        self.assertEqual(self.run_download('redirect'), 1)
        self.assertEqual(Handler.hits, ['/redirect'])

    def test_curl_globbing_is_disabled(self):
        self.assertEqual(self.run_download('opaque?cap={one,two}[1-3]'), 0)
        self.assertEqual(Handler.hits, ['/opaque?cap={one,two}[1-3]'])

    def test_http_error_and_truncated_body_fail(self):
        for path in ['error', 'truncated']:
            with self.subTest(path=path):
                self.assertEqual(self.run_download(path), 1)

    def test_chunked_response_size_limit(self):
        self.assertEqual(self.run_download('chunked', limit=8192), 1)

    def test_deadline_and_cancellation(self):
        for timeout, cancel in [(150, 0), (3000, 150), (3000, 1)]:
            with self.subTest(timeout=timeout, cancel=cancel):
                start = time.monotonic()
                self.assertEqual(self.run_download('slow', timeout=timeout, cancel=cancel), 1)
                self.assertLess(time.monotonic() - start, 1.5)

    def test_proxy_environment_and_curl_config_ignored(self):
        home = self.root / 'home'
        home.mkdir(exist_ok=True)
        (home / '.curlrc').write_text('proxy = "http://127.0.0.1:1"\n')
        env = dict(os.environ, HOME=str(home), ALL_PROXY='http://127.0.0.1:1', http_proxy='http://127.0.0.1:1')
        self.assertEqual(self.run_download(env=env), 0)

    def test_existing_output_not_overwritten(self):
        self.output.write_bytes(b'preserve')
        result = subprocess.run([str(self.exe), 'download', self.origin + 'ok', self.origin,
                                 str(self.output), '1024', '3000', '0'])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.output.read_bytes(), b'preserve')
        self.assertEqual(Handler.hits, [])

    def test_untrusted_tls_certificate_rejected(self):
        key, cert = self.root / 'key.pem', self.root / 'cert.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(cert), '-days', '1',
                        '-subj', '/CN=localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            origin = f'https://127.0.0.1:{server.server_port}/'
            result = subprocess.run([str(self.exe), 'download', origin + 'ok', origin,
                                     str(self.output), '1024', '3000', '0'], timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(self.output.exists())
            self.assertEqual(Handler.hits, [])
        finally:
            server.shutdown()
            server.server_close()


@unittest.skipUnless(os.environ.get('NATIVE_TRANSPORT_BIN'), 'optional full native transport harness not supplied')
class NativeTransportTests(unittest.TestCase):
    setUpClass = classmethod(RestrictedDownloadTests.setUpClass.__func__)
    tearDownClass = classmethod(RestrictedDownloadTests.tearDownClass.__func__)
    setUp = RestrictedDownloadTests.setUp

    def native(self, operation, allow=None):
        result = subprocess.run([os.environ['NATIVE_TRANSPORT_BIN'], operation, self.origin + 'ok',
                                 self.origin if allow is None else allow, str(self.output)], timeout=5)
        self.assertIn(result.returncode, [0, 1])
        return result.returncode

    def test_native_scoped_download(self):
        self.assertEqual(self.native('download'), 0)
        self.assertEqual(self.output.read_bytes(), b'trusted-document-bytes')

    def test_native_upload_operations_stay_denied(self):
        self.output.write_bytes(b'upload-fixture')
        for operation in ['upload', 'upload-file']:
            self.assertEqual(self.native(operation), 1)
        self.assertEqual(Handler.hits, [])

    def test_native_wrong_pool_and_missing_origin_denied(self):
        for origin in ['http://ds-egress-convert.eurooffice.svc.cluster.local:8080/', '']:
            self.assertEqual(self.native('download', allow=origin), 1)
        self.assertEqual(Handler.hits, [])

    def test_native_existing_output_survives_failure_and_destructor(self):
        for origin in [self.origin, '']:
            self.output.write_bytes(b'preserve')
            self.assertEqual(self.native('existing', allow=origin), 1)
            self.assertEqual(self.output.read_bytes(), b'preserve')
        self.assertEqual(Handler.hits, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
