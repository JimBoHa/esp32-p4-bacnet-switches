from contextlib import redirect_stderr
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import verify_dashboard_redirects as checker


def redirect(location, plaintext=False):
    headers = [("Location", location), ("Cache-Control", "no-store"),
               ("Referrer-Policy", "no-referrer"), ("X-Content-Type-Options", "nosniff")]
    if plaintext:
        headers.append(("Connection", "close"))
    return 302, headers, b""


class DashboardRedirectTests(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(host="192.0.2.1", http_port=80, https_port=443,
                                    cert=Path("test-public.pem"), timeout=5)

    def test_complete_check_uses_only_read_methods_and_never_follows(self):
        def reply(args, secure, method, path, headers=None):
            self.assertIn(method, {"GET", "HEAD"})
            self.assertNotIn("Authorization", headers or {})
            if secure and path == "/diagnostics":
                return 200, [], (checker.ROOT / "main/diagnostics_dashboard.html").read_bytes()
            return redirect("/diagnostics" if secure else "https://192.0.2.1/diagnostics", not secure)
        with mock.patch.object(checker, "request", side_effect=reply) as request:
            result = checker.verify(self.args)
        self.assertTrue(result["success"])
        self.assertEqual(request.call_count, 10)
        self.assertFalse(result["tokens_sent"])
        self.assertFalse(result["redirects_followed"])
        self.assertTrue(any(call.args[-1] == {"Host": "untrusted.invalid:1234"}
                            for call in request.call_args_list))

    def test_bad_redirects_fail_closed(self):
        good = redirect("/diagnostics")
        bad_responses = [(301, good[1], b""), (302, good[1], b"unexpected data"),
                         redirect("https://untrusted.invalid/"),
                         (302, good[1] + [("Location", "/diagnostics")], b""),
                         (302, good[1] + [("Set-Cookie", "synthetic=value")], b""),
                         (302, good[1] + [("WWW-Authenticate", "Bearer")], b"")]
        for response in bad_responses:
            with self.subTest(response=response), self.assertRaises(ValueError):
                checker.validate_redirect(response, "/diagnostics")
        with self.assertRaises(ValueError):
            checker.validate_redirect(good, "/diagnostics", plaintext=True)

    def test_unverified_dashboard_stops_before_plaintext(self):
        with mock.patch.object(checker, "request", return_value=(200, [], b"wrong")) as request:
            with self.assertRaises(ValueError): checker.verify(self.args)
            self.assertEqual(request.call_count, 1)
            self.assertTrue(request.call_args.args[1])

    def test_transport_pins_https_and_closes_each_connection(self):
        for secure in (False, True):
            connection = mock.Mock()
            response = connection.getresponse.return_value
            response.status, response.getheaders.return_value, response.read.return_value = 302, [], b""
            with mock.patch.object(checker.ota_client, "_connection", return_value=connection) as tls, \
                 mock.patch.object(checker.http.client, "HTTPConnection", return_value=connection) as plain:
                self.assertEqual(checker.request(self.args, secure, "GET", "/"), (302, [], b""))
                connection.request.assert_called_once_with("GET", "/", headers={})
                connection.close.assert_called_once()
                self.assertEqual(tls.call_count, int(secure))
                self.assertEqual(plain.call_count, int(not secure))
                if secure:
                    tls.assert_called_once_with("192.0.2.1", 443, self.args.cert, 5)

    def test_bad_cli_arguments_never_contact_device(self):
        for extra in (("--host", "https://example.invalid/"), ("--http-port", "443"),
                      ("--timeout", "nan"), ("--https-port", "0"), ("--host", "fe80::1")):
            with mock.patch.object(checker, "request") as request, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): checker.main(["--host", "192.0.2.1", *extra])
                request.assert_not_called()

    def test_server_integration_and_auth_boundary(self):
        server = (checker.ROOT / "main/ota_server.c").read_text()
        redirect_code = (checker.ROOT / "main/dashboard_redirect.c").read_text()
        self.assertIn('config.httpd.max_uri_handlers = 15;', server)
        self.assertIn('config.httpd.ctrl_port = 32768;', server)
        self.assertIn('dashboard_redirect_start(CONFIG_OTA_HTTPS_PORT)', server)
        self.assertLess(server.index('dashboard_redirect_start(CONFIG_OTA_HTTPS_PORT)'),
                        server.index('atomic_store_explicit(&server_ready, true'))
        self.assertIn('dashboard_redirect_stop();', server)
        self.assertNotIn('httpd_req_get_hdr', redirect_code)
        self.assertNotIn('httpd_req_recv', redirect_code)
        for forbidden in ('bearer_token', 'viewer_token', 'config_store', 'esp_restart', 'esp_ota_'):
            self.assertNotIn(forbidden, redirect_code)


if __name__ == "__main__":
    unittest.main()
