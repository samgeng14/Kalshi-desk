#!/usr/bin/env python3
"""The account lock, for the Python side and the old desk (deskd's include/deskd/app/lock.h).

One flock per Kalshi account, at one fixed absolute path, held by every process that can send
orders to that account, so two programs never trade it at once:

    $HOME/Library/Application Support/kalshi-desk/locks/kalshi-<env>.lock     env: prod | demo

Until 2026-10-06 deskd locked <data dir>/kalshi-account.lock with a data dir relative to where it
was started: two deskds from two checkouts both came up, and the old desk, the Python clients
and the scripts took no lock at all (review of week 0). The file is named by the environment,
not by the API key: two keys can trade one account, so a lock per key would let both in.

The holder writes one line into the file, "pid <pid> <holder> since <ISO time>", so whoever is
refused can say who holds it. deskd writes "deskd LIVE" or "deskd DEMO".

    account_lock.py hold ENV HOLDER -- CMD [ARGS...]
        Takes the lock without waiting, then execs CMD with the lock's descriptor inherited, so
        CMD (same pid) holds it for its whole life. desk_supervise.sh starts the old desk's
        binary this way, which never takes the lock itself. Exit 3 when the lock is held: the
        supervisor already reads 3 as "another desk holds the lock; wait".
    account_lock.py status [ENV]
        Who holds it (exit 0 free, 3 held).

From Python: acquire(env, holder) takes it for the life of the process (kalshi_client.py does,
before its first write); env_for_base(url) names the account a REST base trades.
"""
import datetime
import fcntl
import os
import sys
import urllib.parse

DIR_UNDER_HOME = 'Library/Application Support/kalshi-desk/locks'
ENVS = ('prod', 'demo')

_held = {}   # env -> fd, kept open for the life of the process


class LockHeld(RuntimeError):
    pass


def lock_dir():
    home = os.environ.get('HOME', '')
    if not home.startswith('/'):
        raise RuntimeError('HOME is unset or not absolute: no fixed place for the account lock')
    return os.path.join(home, DIR_UNDER_HOME)


def lock_path(env):
    if env not in ENVS:
        raise RuntimeError('account env %r is neither prod nor demo' % (env,))
    return os.path.join(lock_dir(), 'kalshi-%s.lock' % env)


def env_for_base(url):
    """The account a REST or WebSocket base URL trades: 'demo' on Kalshi's demo hosts, None on
    loopback (the fake exchange, which needs no lock), and 'prod' for anything else, so an
    unknown host is treated as the real account rather than let through."""
    host = (urllib.parse.urlparse(url).hostname or '').lower().rstrip('.')
    if host in ('127.0.0.1', 'localhost', '::1'):
        return None
    if host.endswith('.demo.kalshi.co') or host == 'demo-api.kalshi.co':
        return 'demo'
    return 'prod'


def _holder_line(fd):
    try:
        line = os.pread(fd, 160, 0).decode('utf-8', 'replace').strip()
    except OSError:
        line = ''
    return line or 'unknown'


def acquire(env, holder):
    """Takes kalshi-<env>.lock without waiting and keeps it until the process ends. Idempotent
    for one env in one process. Raises LockHeld naming the holder."""
    if env in _held:
        return _held[env]
    path = lock_path(env)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        who = _holder_line(fd)
        os.close(fd)
        raise LockHeld('%s is held by another process that can send orders (%s)' % (os.path.realpath(path), who))
    since = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'
    os.ftruncate(fd, 0)
    os.pwrite(fd, ('pid %d %s since %s\n' % (os.getpid(), holder, since)).encode(), 0)
    _held[env] = fd
    return fd


def main(argv):
    if len(argv) >= 5 and argv[1] == 'hold' and argv[4] == '--' and len(argv) >= 6:
        env, holder, cmd = argv[2], argv[3], argv[5:]
        try:
            fd = acquire(env, holder)
        except LockHeld as e:
            print('account_lock: refusing to start %s: %s' % (cmd[0], e), file=sys.stderr)
            return 3
        except RuntimeError as e:
            print('account_lock: %s' % e, file=sys.stderr)
            return 2
        os.set_inheritable(fd, True)   # exec keeps it open, and with it the lock
        os.execvp(cmd[0], cmd)
    if len(argv) in (2, 3) and argv[1] == 'status':
        rc = 0
        for env in ([argv[2]] if len(argv) == 3 else list(ENVS)):
            path = lock_path(env)
            if not os.path.exists(path):
                print('%s: free (%s)' % (env, path))
                continue
            fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
            try:
                fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
                print('%s: free (%s)' % (env, path))
            except BlockingIOError:
                print('%s: held: %s (%s)' % (env, _holder_line(fd), path))
                rc = 3
            finally:
                os.close(fd)
        return rc
    print('usage: account_lock.py hold ENV HOLDER -- CMD [ARGS...] | account_lock.py status [ENV]', file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
