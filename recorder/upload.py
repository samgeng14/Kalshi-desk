"""Finished Parquet files to Cloudflare R2 (its S3 API), on a write-only token; run hourly by a
systemd timer, apart from the recorder, so an upload failure never stops recording.

A file counts as uploaded only when R2 accepted its PUT with Content-MD5 (R2 refuses a body that
does not match it) and answered with that MD5 as the ETag (a single-part PUT's ETag). A write-only
token cannot read anything back; this is the verification it allows. Each Parquet file goes up with
a sidecar <key>.json holding its manifest row (rows, time range, sha256), so the bucket alone says
what it holds. Every object is written once and never replaced: the prefix is meant to sit under
R2's delete lock (docs/deploy-oracle.md, archive/ locked against deletes).

uploaded.jsonl records each verified upload. A local file is deleted only once it is recorded
there and its hour is older than REC_KEEP_DAYS (default 14). Above REC_DISK_ALERT (default 70%)
disk use, the run reports failure (healthchecks.io's /fail) even if every upload went up.

    python -m recorder.upload        # settings below, from the environment

Credentials: $CREDENTIALS_DIRECTORY/r2-token (or the file R2_TOKEN_FILE names), JSON
{"account_id", "access_key_id", "secret_access_key", "bucket"}; $CREDENTIALS_DIRECTORY/hc-upload-url
(or REC_HC_UPLOAD_URL_FILE), optional. REC_R2_PREFIX (default archive/recorder), REC_R2_ENDPOINT
(default https://<account_id>.r2.cloudflarestorage.com; tests point it at a local server).
"""
import base64
import datetime
import hashlib
import hmac
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from .config import read_secret
from .store import read_manifest


def _hmac(key, msg):
    return hmac.new(key, msg.encode(), hashlib.sha256).digest()


def sign_v4(method, url, headers, payload_hash, access_key, secret_key, region='auto', service='s3', now=None):
    """AWS Signature Version 4 headers for one request (R2 takes region "auto"). `headers` are
    the ones to sign besides host, x-amz-date and x-amz-content-sha256; returned with those and
    Authorization added."""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    amz_date, day = now.strftime('%Y%m%dT%H%M%SZ'), now.strftime('%Y%m%d')
    u = urllib.parse.urlsplit(url)
    h = {k.lower(): ' '.join(str(v).split()) for k, v in headers.items()}
    h['host'] = u.netloc
    h['x-amz-date'] = amz_date
    h['x-amz-content-sha256'] = payload_hash
    names = sorted(h)
    query = '&'.join('%s=%s' % (urllib.parse.quote(k, safe='-_.~'), urllib.parse.quote(v, safe='-_.~'))
                     for k, v in sorted(urllib.parse.parse_qsl(u.query, keep_blank_values=True)))
    canonical = '\n'.join([method, u.path or '/', query, ''.join('%s:%s\n' % (n, h[n]) for n in names),
                           ';'.join(names), payload_hash])
    scope = '%s/%s/%s/aws4_request' % (day, region, service)
    to_sign = '\n'.join(['AWS4-HMAC-SHA256', amz_date, scope, hashlib.sha256(canonical.encode()).hexdigest()])
    key = _hmac(_hmac(_hmac(_hmac(('AWS4' + secret_key).encode(), day), region), service), 'aws4_request')
    signature = hmac.new(key, to_sign.encode(), hashlib.sha256).hexdigest()
    h['authorization'] = 'AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s' % (
        access_key, scope, ';'.join(names), signature)
    return h


def object_url(endpoint, bucket, key):
    # each path segment percent-encoded once, as S3 canonicalizes it ('=' in date=... too)
    return '%s/%s/%s' % (endpoint.rstrip('/'), urllib.parse.quote(bucket, safe=''),
                         urllib.parse.quote(key, safe='/-_.~'))


class R2:
    def __init__(self, creds, endpoint=None, timeout=60, ca_file=''):
        self.access_key = creds['access_key_id']
        self.secret_key = creds['secret_access_key']
        self.bucket = creds['bucket']
        self.endpoint = endpoint or 'https://%s.r2.cloudflarestorage.com' % creds['account_id']
        self.timeout = timeout
        self.ctx = None
        if ca_file:
            import ssl
            self.ctx = ssl.create_default_context()
            self.ctx.load_verify_locations(ca_file)

    def put(self, key, body, content_type):
        """PUT one object; (verified, detail). Verified: 2xx and the ETag is the body's MD5."""
        md5 = hashlib.md5(body).digest()
        url = object_url(self.endpoint, self.bucket, key)
        headers = sign_v4('PUT', url, {'content-md5': base64.b64encode(md5).decode(), 'content-type': content_type,
                                       'content-length': str(len(body))},
                          hashlib.sha256(body).hexdigest(), self.access_key, self.secret_key)
        req = urllib.request.Request(url, data=body, method='PUT')
        for k, v in headers.items():
            if k != 'host':
                req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=self.ctx) as r:
                etag = (r.headers.get('ETag') or '').strip('"').lower()
        except urllib.error.HTTPError as e:
            return False, 'HTTP %d: %s' % (e.code, e.read()[:200].decode('utf-8', 'replace'))
        except (urllib.error.URLError, OSError) as e:
            return False, str(e)[:200]
        if etag != md5.hex():
            return False, 'ETag %s is not the MD5 %s' % (etag or '(none)', md5.hex())
        return True, etag


def log(msg):
    print('%s upload: %s' % (datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), msg),
          flush=True)


def read_uploaded(root):
    p = os.path.join(root, 'uploaded.jsonl')
    out = {}
    if os.path.exists(p):
        with open(p, encoding='utf-8') as f:
            for line in f:
                try:
                    row = json.loads(line)
                    out[row['path']] = row
                except (ValueError, KeyError):
                    pass
    return out


def run(root, r2, prefix, keep_days=14, disk_alert=0.70, now_s=None):
    """One pass; (ok, summary). ok: every finished file is up and the disk is under the alert."""
    now_s = time.time() if now_s is None else now_s
    uploaded = read_uploaded(root)
    ok, up, failed = True, 0, 0
    for entry in read_manifest(root):
        path = entry['path']
        if path in uploaded:
            continue
        full = os.path.join(root, path)
        if not os.path.exists(full):
            log('%s is in the manifest but not on disk' % path)
            ok = False
            continue
        with open(full, 'rb') as f:
            body = f.read()
        if hashlib.sha256(body).hexdigest() != entry['sha256']:
            log('%s does not match its manifest sha256: not uploaded' % path)
            ok, failed = False, failed + 1
            continue
        key = '%s/%s' % (prefix.strip('/'), path[len('parquet/'):] if path.startswith('parquet/') else path)
        good, detail = r2.put(key, body, 'application/vnd.apache.parquet')
        if good:
            good, detail = r2.put(key + '.json', json.dumps(entry, separators=(',', ':')).encode(), 'application/json')
        if not good:
            log('%s: %s' % (path, detail))
            ok, failed = False, failed + 1
            continue
        row = {'path': path, 'key': key, 'md5': detail, 'uploaded_s': int(now_s)}
        with open(os.path.join(root, 'uploaded.jsonl'), 'a', encoding='utf-8') as f:
            f.write(json.dumps(row, separators=(',', ':')) + '\n')
            f.flush()
            os.fsync(f.fileno())
        uploaded[path] = row
        up += 1
    # local retention: only what is up, and older than keep_days
    removed = 0
    for entry in read_manifest(root):
        full = os.path.join(root, entry['path'])
        if entry['path'] in uploaded and os.path.exists(full) and entry['t_max_us'] / 1e6 < now_s - keep_days * 86400:
            os.unlink(full)
            removed += 1
    du = shutil.disk_usage(root)
    used = 1 - du.free / du.total if du.total else 0
    if used > disk_alert:
        log('disk %.0f%% used, over the %.0f%% alert' % (used * 100, disk_alert * 100))
        ok = False
    return ok, '%d uploaded, %d failed, %d removed locally, disk %.0f%%' % (up, failed, removed, used * 100)


def ping(url, ok):
    if not url:
        return
    try:
        urllib.request.urlopen(url if ok else url.rstrip('/') + '/fail', timeout=10).read()
    except Exception as e:   # the check then goes red by itself
        log('healthchecks: %s' % e)


def main():
    env = os.environ
    root = env.get('REC_DATA_DIR', '/var/lib/deskd-recorder')
    token = read_secret(env, 'r2-token', 'R2_TOKEN_FILE')
    if not token:
        print('upload: no r2-token credential (or R2_TOKEN_FILE)', file=sys.stderr)
        return 2
    try:
        creds = json.loads(token)
        r2 = R2(creds, env.get('REC_R2_ENDPOINT') or None, ca_file=env.get('REC_CA_FILE', ''))
    except (ValueError, KeyError) as e:
        print('upload: the r2-token credential is not {account_id, access_key_id, secret_access_key, bucket}: %s' % e,
              file=sys.stderr)
        return 2
    hc = read_secret(env, 'hc-upload-url', 'REC_HC_UPLOAD_URL_FILE')
    ok, summary = run(root, r2, env.get('REC_R2_PREFIX', 'archive/recorder'), int(env.get('REC_KEEP_DAYS', '14')),
                      float(env.get('REC_DISK_ALERT', '0.70')))
    log(summary)
    ping(hc.decode().strip() if hc else '', ok)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
