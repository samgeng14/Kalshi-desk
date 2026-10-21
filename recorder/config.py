"""Settings: the environment (systemd's EnvironmentFile on the VM) and the credentials directory.

Kalshi's hosts have no default: the VM's recorder.env names production (read-only key), a local
soak names demo or the fake. (The public scoreboards, ESPN's and NCAA.com's, have one: there is
only one of each.) Every series list is explicit; 'off' empties a list, and an empty list
records nothing. Secrets come from $CREDENTIALS_DIRECTORY (systemd LoadCredential) or,
for a run by hand, from files named in the environment; never from the environment itself and
never from a command line (both are visible in ps).
"""
import os
import re
from dataclasses import dataclass, field
from urllib.parse import urlparse

# The 15-minute financial series (deskd's strat/fair.cpp underlyings) and the weather highs
# deskd's band lane quotes (app/config.h band_series). Books, trades and strikes for these are
# what the LIP-15 replication gate and the band replay need (plan section 5).
LIP_SERIES = ['KXGOLD15M', 'KXSILVER15M', 'KXWTI15M', 'KXNATGAS15M', 'KXCOPPER15M',
              'KXPLATINUM15M', 'KXPALLADIUM15M', 'KXEURUSD15M', 'KXGBPUSD15M', 'KXUSDJPY15M']
WEATHER_SERIES = ['KXHIGHNY', 'KXHIGHCHI', 'KXHIGHMIA', 'KXHIGHAUS', 'KXHIGHLAX']

# Plan 5(d), the thin college and minor-league games: every game of these is recorded (books and
# trades on the socket, books by REST, scores). TO CONFIRM by the operator on kalshi.com before
# relying on them; a wrong ticker lists no markets and records nothing (it does no harm):
#   KXNCAAWBGAME      women's college basketball   in deskd (ref/espn.cpp) and docs/edge_results.json
#   KXNCAAWVMATCH     women's college volleyball   docs/edge_results.json (275 matches, Sep-Oct 2026)
#   KXNCAAHOCKEYGAME  college hockey               docs/edge_results.json (last season's games)
#   KXTTSTARMATCH     table tennis (TT Star Series) the demo's listing (tests/deskd/fixtures); other
#                                                    table tennis series may exist
#   KXAHLGAME         AHL                          a guess from the venue's naming
#   KXLIIGAGAME       Liiga                        docs/edge_results.json (83 events, Sep-Oct 2026)
#   KXSHLGAME         SHL                          a guess
#   KXCS2GAME         CS2                          a guess (esports may be per map or per match)
COLLEGE_SERIES = ['KXNCAAWBGAME', 'KXNCAAWVMATCH', 'KXNCAAHOCKEYGAME', 'KXTTSTARMATCH', 'KXAHLGAME',
                  'KXLIIGAGAME', 'KXSHLGAME', 'KXCS2GAME']
# the control: men's college basketball, only its thinnest tenth by pregame volume (college.py)
CONTROL_SERIES = ['KXNCAAMBGAME']
# the game group (games.py): the NBA's game winners, books and trades on the socket only inside
# each game's window (REC_GAME_PRE_S before its scheduled start to REC_LINGER_S after its close)
GAME_SERIES = ['KXNBAGAME']
# scoreboards (scores.py): ESPN 'sport/league[:groups]'; groups=50 is Division I for basketball.
# Volleyball's and hockey's league names are ESPN's site paths, TO CONFIRM (ESPN has no AHL, Liiga,
# SHL, CS2 or table tennis; their scores are not recorded). basketball/nba is the game group's,
# the path deskd's ref/espn.cpp reads.
ESPN_LEAGUES = ['basketball/womens-college-basketball:50', 'basketball/mens-college-basketball:50',
                'volleyball/womens-college-volleyball', 'hockey/mens-college-hockey', 'basketball/nba']
# NCAA.com's scoreboard JSON, 'sport/division': unofficial, TO CONFIRM
NCAA_SPORTS = ['basketball-women/d1', 'basketball-men/d1', 'volleyball-women/d1', 'icehockey-men/d1']
INCENTIVE_TYPES = ['liquidity', 'volume']

OFF = ('off', 'none')
TICKER = re.compile(r'[A-Z0-9]+')


def _uncomment(v):
    """systemd's EnvironmentFile keeps 'REC_LEAD_S=1200   # why' whole; the comment is cut here,
    so an uncommented example line cannot stop the recorder."""
    for mark in (' #', '\t#'):
        if mark in v:
            v = v.split(mark, 1)[0]
    return v.strip()


def _list(v, default):
    if v is None or not v.strip():
        return list(default)
    return [x.strip() for x in v.split(',') if x.strip()]


def _optlist(v, default):
    """A list that may be switched off: 'off' (or 'none') is no entries, never 'everything'."""
    if v is not None and v.strip().lower() in OFF:
        return []
    return _list(v, default)


def _series(name, items):
    for s in items:
        if not TICKER.fullmatch(s):
            raise ValueError('%s: %r is not a series ticker (A-Z, 0-9)' % (name, s))
    return items


def _int(env, name, default, lo, hi):
    v = env.get(name)
    n = default if v is None or v == '' else int(v)
    if not lo <= n <= hi:
        raise ValueError('%s must be %d..%d, not %d' % (name, lo, hi, n))
    return n


def _float(env, name, default, lo, hi):
    v = env.get(name)
    n = default if v is None or v == '' else float(v)
    if not lo <= n <= hi:
        raise ValueError('%s must be %g..%g, not %g' % (name, lo, hi, n))
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
    # plan 5(d): college and thin-sport games
    college_series: list = field(default_factory=list)
    control_series: list = field(default_factory=list)
    control_frac: float = 0.1           # the thinnest tenth of the control's games
    control_pick_h: int = 15            # picked at 15 UTC (10-11 ET), before the first tip-offs
    college_discover_s: int = 3600      # the college series are listed hourly
    lifecycle: bool = True              # market_lifecycle_v2 on the recorded and college markets
    # the game group (games.py): listed hourly, its books on the socket only around each game
    game_series: list = field(default_factory=list)
    game_pre_s: int = 7200              # from this long before a game's scheduled start
    # public REST
    rest_rps: int = 5                   # discovery, trade backfill, incentives, the daily listing
    rest_book_rps: int = 5              # the REST books
    rest_book_s: float = 4.0            # every college market's top of book this often
    rest_book_hot_s: int = 300          # and its depth while its top moved this recently
    rest_fallback_s: int = 300          # socket books down this long: REST polls the book series too
    backfill_s: int = 3600              # trade backfill over the last hour, this often
    incentives_s: int = 3600
    incentive_types: list = field(default_factory=list)
    incentive_status: str = 'active'
    markets_daily_h: int = 12           # the day's GET /markets sweep at or after this UTC hour
    markets_daily_pages: int = 500
    # scores
    espn_url: str = 'https://site.api.espn.com/apis/site/v2/sports'
    espn_leagues: list = field(default_factory=list)
    ncaa_url: str = 'https://data.ncaa.com/casablanca/scoreboard'
    ncaa_sports: list = field(default_factory=list)
    scores_s: float = 4.0               # while a game is on
    scores_idle_s: float = 60.0         # otherwise


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
    env = {k: _uncomment(v) if k.startswith(('REC_', 'KALSHI_')) else v for k, v in env.items()}
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
    _series('REC_BOOK_SERIES', s.book_series)
    s.college_series = _series('REC_COLLEGE_SERIES', _optlist(env.get('REC_COLLEGE_SERIES'), COLLEGE_SERIES))
    s.control_series = _series('REC_CONTROL_SERIES', _optlist(env.get('REC_CONTROL_SERIES'), CONTROL_SERIES))
    s.game_series = _series('REC_GAME_SERIES', _optlist(env.get('REC_GAME_SERIES'), GAME_SERIES))
    every = s.book_series + s.college_series + s.control_series + s.game_series
    if len(set(every)) != len(every):
        raise ValueError('a series is in more than one of REC_BOOK_SERIES, REC_COLLEGE_SERIES, REC_CONTROL_SERIES, '
                         'REC_GAME_SERIES')
    s.game_pre_s = _int(env, 'REC_GAME_PRE_S', s.game_pre_s, 0, 86400)
    s.control_frac = _float(env, 'REC_CONTROL_FRAC', s.control_frac, 0.01, 1.0)
    # before 5 UTC the US date is still the day before the UTC date, so a pick then ranks the next
    # US day's games a day early, on thinner volume; allowed, but 15 is the default for a reason
    s.control_pick_h = _int(env, 'REC_CONTROL_PICK_UTC_H', s.control_pick_h, 0, 23)
    s.college_discover_s = _int(env, 'REC_COLLEGE_DISCOVER_S', s.college_discover_s, 60, 86400)
    s.lifecycle = env.get('REC_LIFECYCLE', '1').strip() not in ('0', 'off', 'no', 'false')
    s.rest_rps = _int(env, 'REC_REST_RPS', s.rest_rps, 1, 15)
    s.rest_book_rps = _int(env, 'REC_REST_BOOK_RPS', s.rest_book_rps, 1, 15)
    s.rest_book_s = _float(env, 'REC_REST_BOOK_S', s.rest_book_s, 1.0, 60.0)
    s.rest_book_hot_s = _int(env, 'REC_REST_BOOK_HOT_S', s.rest_book_hot_s, 0, 86400)
    s.rest_fallback_s = _int(env, 'REC_REST_FALLBACK_S', s.rest_fallback_s, 0, 86400)
    s.backfill_s = _int(env, 'REC_BACKFILL_S', s.backfill_s, 60, 86400)
    s.incentives_s = _int(env, 'REC_INCENTIVES_S', s.incentives_s, 60, 86400)
    s.incentive_types = _optlist(env.get('REC_INCENTIVE_TYPES'), INCENTIVE_TYPES)
    s.incentive_status = env.get('REC_INCENTIVE_STATUS', '').strip() or s.incentive_status
    s.markets_daily_h = _int(env, 'REC_MARKETS_DAILY_UTC_H', s.markets_daily_h, 0, 23)
    s.markets_daily_pages = _int(env, 'REC_MARKETS_DAILY_PAGES', s.markets_daily_pages, 1, 5000)
    s.espn_url = check_url(env.get('REC_ESPN_URL', '') or s.espn_url, ('https', 'http'), 'REC_ESPN_URL').rstrip('/')
    s.espn_leagues = _optlist(env.get('REC_ESPN_LEAGUES'), ESPN_LEAGUES)
    s.ncaa_url = check_url(env.get('REC_NCAA_URL', '') or s.ncaa_url, ('https', 'http'), 'REC_NCAA_URL').rstrip('/')
    s.ncaa_sports = _optlist(env.get('REC_NCAA_SPORTS'), NCAA_SPORTS)
    s.scores_s = _float(env, 'REC_SCORES_S', s.scores_s, 3.0, 60.0)
    s.scores_idle_s = _float(env, 'REC_SCORES_IDLE_S', s.scores_idle_s, s.scores_s, 3600.0)
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
