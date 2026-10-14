"""Settings: the environment (systemd's EnvironmentFile on the VM) and the credentials directory.

Hosts have no default: the VM's recorder.env names production (read-only key), a local soak
names demo or the fake. Secrets come from $CREDENTIALS_DIRECTORY (systemd LoadCredential) or,
for a run by hand, from files named in the environment; never from the environment itself and
never from a command line (both are visible in ps).
"""
import os
from dataclasses import dataclass, field
from urllib.parse import urlparse

# The 15-minute financial series (deskd's strat/fair.cpp underlyings) and the weather highs
# deskd's band lane quotes (app/config.h band_series). Books, trades and strikes for these are
# what the LIP-15 replication gate and the band replay need (plan section 5).
LIP_SERIES = ['KXGOLD15M', 'KXSILVER15M', 'KXWTI15M', 'KXNATGAS15M', 'KXCOPPER15M',
              'KXPLATINUM15M', 'KXPALLADIUM15M', 'KXEURUSD15M', 'KXGBPUSD15M', 'KXUSDJPY15M']
WEATHER_SERIES = ['KXHIGHNY', 'KXHIGHCHI', 'KXHIGHMIA', 'KXHIGHAUS', 'KXHIGHLAX']


def _list(v, default):
    if v is None or not v.strip():
        return list(default)
    return [x.strip() for x in v.split(',') if x.strip()]


def _int(env, name, default, lo, hi):
    v = env.get(name)
    n = default if v is None or v == '' else int(v)
    if not lo <= n <= hi:
        raise ValueError('%s must be %d..%d, not %d' % (name, lo, hi, n))
    return n


def is_loopback(host):
    return host in ('127.0.0.1', '::1', 'localhost')


def check_url(url, schemes, name):
    u = urlparse(url)
    if u.scheme not in schemes or not u.hostname:
        raise ValueError('%s must be %s://host/...: %r' % (name, '/'.join(schemes), url))
    # plain http/ws only to loopback (the fake exchange in tests)
    if u.scheme in ('http', 'ws') and not is_loopback(u.hostname):
        raise ValueError('%s: plain %s only to loopback' % (name, u.scheme))
    return url


@dataclass
class Settings:
    rest_url: str                       # .../trade-api/v2
    ws_url: str                         # .../trade-api/ws/v2
    data_dir: str
    book_series: list = field(default_factory=list)
    lead_s: int = 1200                  # subscribe a window this long before it opens
    linger_s: int = 300                 # and keep it this long after it closes
    discover_s: int = 30                # REST discovery period
    settled_s: int = 300                # how often closed/settled rows are read for results
    bucket_s: int = 3600                # one Parquet file per table per hour
    ca_file: str = ''                   # extra CA for loopback tests only
    key_id: str = ''
    key_pem: bytes = b''
    hc_url: str = ''


def read_secret(env, cred_name, file_var):
    """A secret from systemd's credentials directory, else from the file a variable names."""
    d = env.get('CREDENTIALS_DIRECTORY')
    if d and os.path.exists(os.path.join(d, cred_name)):
        with open(os.path.join(d, cred_name), 'rb') as f:
            return f.read()
    p = env.get(file_var)
    if p:
        with open(os.path.expanduser(p), 'rb') as f:
            return f.read()
    return None


def load(env=None):
    env = os.environ if env is None else env
    rest = env.get('KALSHI_REST', '')
    ws = env.get('KALSHI_WS', '')
    if not rest or not ws:
        raise ValueError('KALSHI_REST and KALSHI_WS must be set (the recorder has no default host)')
    check_url(rest, ('https', 'http'), 'KALSHI_REST')
    check_url(ws, ('wss', 'ws'), 'KALSHI_WS')
    s = Settings(rest_url=rest.rstrip('/'), ws_url=ws.rstrip('/'),
                 data_dir=env.get('REC_DATA_DIR', '/var/lib/deskd-recorder'))
    s.book_series = _list(env.get('REC_BOOK_SERIES'), LIP_SERIES + WEATHER_SERIES)
    s.lead_s = _int(env, 'REC_LEAD_S', s.lead_s, 0, 86400)
    s.linger_s = _int(env, 'REC_LINGER_S', s.linger_s, 0, 86400)
    s.discover_s = _int(env, 'REC_DISCOVER_S', s.discover_s, 1, 3600)
    s.settled_s = _int(env, 'REC_SETTLED_S', s.settled_s, 1, 86400)
    s.bucket_s = _int(env, 'REC_BUCKET_S', s.bucket_s, 1, 3600)
    if 3600 % s.bucket_s:
        raise ValueError('REC_BUCKET_S must divide an hour')
    s.ca_file = env.get('REC_CA_FILE', '')
    if s.ca_file and not all(is_loopback(urlparse(u).hostname) for u in (rest, ws)):
        raise ValueError('REC_CA_FILE is for loopback tests only')
    key_id = read_secret(env, 'kalshi-key-id', 'KALSHI_KEY_ID_FILE')
    pem = read_secret(env, 'kalshi-key.pem', 'KALSHI_KEY_FILE')
    if not key_id or not pem:
        raise ValueError('no Kalshi key: kalshi-key-id and kalshi-key.pem credentials '
                         '(or KALSHI_KEY_ID_FILE and KALSHI_KEY_FILE)')
    s.key_id = key_id.decode().strip()
    s.key_pem = pem
    hc = read_secret(env, 'hc-url', 'REC_HC_URL_FILE')
    s.hc_url = hc.decode().strip() if hc else ''
    return s
