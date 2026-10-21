"""What the recorder's tests share: fixtures, which optional packages are installed (a bare python3,
as ctest may run, has none of them: the tests that need one skip), a store's spooled rows, and a
fake REST under the real Venue (no network anywhere)."""
import glob
import importlib.util
import json
import os

from recorder import polls

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, 'fixtures')
ROOT = os.path.dirname(os.path.dirname(HERE))


def have(module):
    return importlib.util.find_spec(module) is not None


HAVE_PYARROW = have('pyarrow')
HAVE_CRYPTO = have('cryptography')
HAVE_WEBSOCKETS = have('websockets')
HAVE_DUCKDB = have('duckdb')


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding='utf-8') as f:
        return json.load(f)


def fixture_lines(name):
    with open(os.path.join(FIXTURES, name), encoding='utf-8') as f:
        return [json.loads(line) for line in f if line.strip()]


def spooled(root, table):
    """Every row a store spooled for a table (flush the store first)."""
    out = []
    for path in sorted(glob.glob(os.path.join(root, 'spool', table, '*.jsonl'))):
        with open(path, encoding='utf-8') as f:
            out += [json.loads(line) for line in f if line.strip()]
    return out


class FakeRest:
    """kalshi.Rest's one method, answered by handler(route, params); every call kept."""

    def __init__(self, handler):
        self.handler = handler
        self.calls = []

    def get(self, route, params=None):
        params = {k: v for k, v in (params or {}).items() if v not in (None, '')}
        self.calls.append((route, params))
        return self.handler(route, params)


def fake_venue(handler, rate=1000.0):
    rest = FakeRest(handler)
    return polls.Venue(rest, polls.RateLimiter(rate)), rest


def gap_log():
    """A gap function that keeps its rows, as Recorder.gap's signature."""
    rows = []

    def gap(source, kind, detail, from_us=None, to_us=None, key=None, every_s=0):
        rows.append({'source': source, 'kind': kind, 'detail': detail, 'from_us': from_us, 'to_us': to_us})
    gap.rows = rows
    return gap
