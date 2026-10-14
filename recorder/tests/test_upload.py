"""The R2 uploader: SigV4 against botocore's, verified PUTs against a local fake R2, the
uploaded log and local retention."""
import base64
import datetime
import hashlib
import http.server
import json
import os
import tempfile
import threading
import importlib.util
import unittest

from recorder import upload
from recorder.store import Store

CREDS = {'account_id': 'acct', 'access_key_id': 'AKIDEXAMPLE', 'secret_access_key': 'wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY',
         'bucket': 'desk-rec'}
WHEN = datetime.datetime(2026, 10, 10, 16, 5, 7, tzinfo=datetime.timezone.utc)


class SigV4Test(unittest.TestCase):
    def test_against_botocore(self):
        try:
            from unittest import mock
            from botocore.auth import S3SigV4Auth
            from botocore.awsrequest import AWSRequest
            from botocore.credentials import Credentials
        except ImportError:
            self.skipTest('botocore is not installed (a test-only reference)')
        body = b'parquet bytes'
        key = 'archive/recorder/kalshi_ws/date=2026-10-10/kalshi_ws-20261010T15.parquet'
        url = upload.object_url('https://acct.r2.cloudflarestorage.com', 'desk-rec', key)
        md5 = base64.b64encode(hashlib.md5(body).digest()).decode()
        ours = upload.sign_v4('PUT', url, {'content-md5': md5, 'content-type': 'application/vnd.apache.parquet'},
                              hashlib.sha256(body).hexdigest(), CREDS['access_key_id'], CREDS['secret_access_key'], now=WHEN)
        req = AWSRequest(method='PUT', url=url, data=body,
                         headers={'Content-MD5': md5, 'Content-Type': 'application/vnd.apache.parquet'})
        auth = S3SigV4Auth(Credentials(CREDS['access_key_id'], CREDS['secret_access_key']), 's3', 'auto')

        class Fixed(datetime.datetime):
            @classmethod
            def utcnow(cls):
                return WHEN.replace(tzinfo=None)

            @classmethod
            def now(cls, tz=None):
                return WHEN if tz else WHEN.replace(tzinfo=None)

        with mock.patch('botocore.auth.datetime.datetime', Fixed):
            auth.add_auth(req)
        theirs = req.headers['Authorization']
        self.assertEqual(ours['authorization'].split('Signature=')[0], theirs.split('Signature=')[0])
        self.assertEqual(ours['authorization'], theirs)


class FakeR2(http.server.BaseHTTPRequestHandler):
    objects, bad_etag = {}, False

    def do_PUT(self):
        body = self.rfile.read(int(self.headers['Content-Length']))
        h = {k.lower(): v for k, v in self.headers.items()}
        signed = upload.sign_v4('PUT', 'http://%s%s' % (h['host'], self.path),
                                {k: h[k] for k in ('content-md5', 'content-type', 'content-length')},
                                h['x-amz-content-sha256'], CREDS['access_key_id'], CREDS['secret_access_key'],
                                now=datetime.datetime.strptime(h['x-amz-date'], '%Y%m%dT%H%M%SZ').replace(tzinfo=datetime.timezone.utc))
        if signed['authorization'] != h['authorization'] or h['x-amz-content-sha256'] != hashlib.sha256(body).hexdigest():
            self.send_response(403); self.end_headers(); return
        if base64.b64decode(h['content-md5']) != hashlib.md5(body).digest():
            self.send_response(400); self.end_headers(); return   # BadDigest
        FakeR2.objects[self.path] = body
        self.send_response(200)
        self.send_header('ETag', '"%s"' % ('0' * 32 if FakeR2.bad_etag else hashlib.md5(body).hexdigest()))
        self.end_headers()

    def log_message(self, *a):
        pass


HAVE_PYARROW = importlib.util.find_spec('pyarrow') is not None


@unittest.skipUnless(HAVE_PYARROW, 'pyarrow is not installed (pip install -r recorder/requirements.txt)')
class RunTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        FakeR2.objects, FakeR2.bad_etag = {}, False
        self.srv = http.server.HTTPServer(('127.0.0.1', 0), FakeR2)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.r2 = upload.R2(CREDS, 'http://127.0.0.1:%d' % self.srv.server_port)
        # an hour of rows, compacted
        s = Store(self.root, 3600, lambda m: None)
        t0 = 1791648000 * 1_000_000
        for i in range(3):
            s.write('kalshi_ws', {'recv_us': t0 + i, 'mono_us': i, 'conn': 1, 'type': 'pyth_value', 'sid': 1, 'seq': None,
                                  'ticker': 'X', 'raw': '{}'})
        s.flush(fsync=True)
        self.written = s.roll(t0 + 3600 * 1_000_000 + 6_000_000)
        s.close()
        self.t0 = t0

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.dir.cleanup()

    def test_upload_verify_and_keep(self):
        ok, summary = upload.run(self.root, self.r2, 'archive/recorder', now_s=self.t0 / 1e6 + 7200, disk_alert=1.0)
        self.assertTrue(ok, summary)
        keys = sorted(FakeR2.objects)
        self.assertEqual(keys, ['/desk-rec/archive/recorder/kalshi_ws/date%3D2026-10-10/kalshi_ws-20261010T16.parquet',
                                '/desk-rec/archive/recorder/kalshi_ws/date%3D2026-10-10/kalshi_ws-20261010T16.parquet.json'])
        with open(self.written[0], 'rb') as f:
            self.assertEqual(FakeR2.objects[keys[0]], f.read())
        self.assertEqual(json.loads(FakeR2.objects[keys[1]])['rows'], 3)
        # a second run sends nothing again; the file stays until it is 14 days old
        FakeR2.objects = {}
        ok, summary = upload.run(self.root, self.r2, 'archive/recorder', now_s=self.t0 / 1e6 + 7200, disk_alert=1.0)
        self.assertTrue(ok and not FakeR2.objects and os.path.exists(self.written[0]), summary)
        ok, summary = upload.run(self.root, self.r2, 'archive/recorder', now_s=self.t0 / 1e6 + 15 * 86400, disk_alert=1.0)
        self.assertTrue(ok, summary)
        self.assertFalse(os.path.exists(self.written[0]))

    def test_unverified_is_not_uploaded(self):
        FakeR2.bad_etag = True
        ok, summary = upload.run(self.root, self.r2, 'archive/recorder', now_s=self.t0 / 1e6 + 15 * 86400, disk_alert=1.0)
        self.assertFalse(ok)
        self.assertTrue(os.path.exists(self.written[0]))   # never deleted unless verified up
        self.assertFalse(os.path.exists(os.path.join(self.root, 'uploaded.jsonl')))
        # a wrong secret: R2 refuses it
        FakeR2.bad_etag = False
        wrong = upload.R2(dict(CREDS, secret_access_key='nope'), self.r2.endpoint)
        ok, _ = upload.run(self.root, wrong, 'archive/recorder', disk_alert=1.0)
        self.assertFalse(ok)

    def test_corrupted_local_file_is_not_uploaded(self):
        with open(self.written[0], 'ab') as f:
            f.write(b'x')
        ok, _ = upload.run(self.root, self.r2, 'archive/recorder', disk_alert=1.0)
        self.assertFalse(ok)
        self.assertEqual(FakeR2.objects, {})

    def test_disk_alert(self):
        ok, summary = upload.run(self.root, self.r2, 'archive/recorder', disk_alert=0.0)
        self.assertFalse(ok)
        self.assertIn('1 uploaded', summary)


if __name__ == '__main__':
    unittest.main()
