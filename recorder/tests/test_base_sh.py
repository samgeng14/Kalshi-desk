"""deploy/base.sh (chrony, swap, the journald cap, security-only upgrades) run under a temporary
root with every system command stubbed: plan mode changes nothing, --apply does each thing once,
a second --apply does nothing, a file you changed is kept (ours goes beside it as .new), and chrony
is not installed if apt would remove more than timesyncd (which then stays on)."""
import os
import shutil
import subprocess
import tempfile
import unittest

from recorder.tests.util import ROOT

DEPLOY = os.path.join(ROOT, 'recorder', 'deploy')


def slurp(path):
    with open(path) as f:
        return f.read()

STUBS = {
    'dpkg': '[ "$1" = -s ] && [ -e "$STUB_STATE/pkg-$2" ] && exit 0\nexit 1\n',
    'apt-get': 'for a in "$@"; do [ "$a" = -s ] && { cat "$STUB_STATE/apt-sim" 2>/dev/null; exit 0; }; done\n'
               'last=; for a in "$@"; do last=$a; done\n'
               'case " $* " in *" install "*) touch "$STUB_STATE/pkg-$last";; esac\nexit 0\n',
    'systemctl': 'case "$1" in\n'
                 '  is-enabled|is-active) u=$2; [ "$u" = --quiet ] && u=$3; [ -e "$STUB_STATE/on-$u" ]; exit $? ;;\n'
                 '  enable) [ "$2" = --now ] && touch "$STUB_STATE/on-$3"\n'
                 '          [ "$3" = swapfile.swap ] && echo "/swapfile file 16M 0B -2" > "$STUB_STATE/swaps"; exit 0 ;;\n'
                 '  disable) [ "$2" = --now ] && rm -f "$STUB_STATE/on-$3"; exit 0 ;;\n'
                 '  *) exit 0 ;;\nesac\n',
    'swapon': 'cat "$STUB_STATE/swaps" 2>/dev/null; exit 0\n',
    'blkid': 'cat "$STUB_STATE/blkid" 2>/dev/null; exit 0\n',
    'fallocate': 'for a in "$@"; do last=$a; done; : > "$last"\n',
    'mkswap': 'exit 0\n',
    'install': 'while [ $# -gt 2 ]; do shift; done\nmkdir -p "$(dirname "$2")" && cp "$1" "$2"\n',
    'sudo': 'exec "$@"\n',
}
MUTATING = ('install ', 'fallocate', 'mkswap', 'systemctl enable', 'systemctl disable', 'systemctl restart',
            'systemctl daemon-reload')


@unittest.skipUnless(shutil.which('bash'), 'bash is needed')
class BaseShTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.root = os.path.join(self.dir, 'root')
        self.state = os.path.join(self.dir, 'state')
        self.stubs = os.path.join(self.dir, 'bin')
        self.log = os.path.join(self.dir, 'calls.log')
        for d in (self.root, self.state, self.stubs, os.path.join(self.dir, 'tmp'), os.path.join(self.root, 'etc')):
            os.makedirs(d, exist_ok=True)
        for name, body in STUBS.items():
            p = os.path.join(self.stubs, name)
            with open(p, 'w') as f:
                f.write('#!/bin/sh\necho "%s $*" >> "$STUB_LOG"\n%s' % (name, body))
            os.chmod(p, 0o755)
        self.on('systemd-timesyncd')   # an Ubuntu image: timesyncd running, apt would swap it for chrony
        self.write_state('apt-sim', 'Remv systemd-timesyncd [255.4-1ubuntu8]\nInst chrony (4.5-1ubuntu4 Ubuntu:24.04/noble)\n')

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def on(self, unit):
        open(os.path.join(self.state, 'on-' + unit), 'w').close()

    def write_state(self, name, text):
        with open(os.path.join(self.state, name), 'w') as f:
            f.write(text)

    def path(self, rel):
        return os.path.join(self.root, rel)

    def read(self, rel):
        return slurp(self.path(rel))

    def run_base(self, *args):
        if os.path.exists(self.log):
            os.unlink(self.log)
        env = dict(os.environ, PATH=self.stubs + os.pathsep + os.environ.get('PATH', '/usr/bin:/bin'),
                   STUB_LOG=self.log, STUB_STATE=self.state, DESKD_BASE_ROOT=self.root, SWAP_MB='16',
                   TMPDIR=os.path.join(self.dir, 'tmp'))
        p = subprocess.run(['bash', os.path.join(DEPLOY, 'base.sh')] + list(args), env=env, capture_output=True,
                           text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        calls = slurp(self.log).splitlines() if os.path.exists(self.log) else []
        return p.stdout, calls

    def files(self):
        out = []
        for d, _, fs in os.walk(self.root):
            out += [os.path.relpath(os.path.join(d, f), self.root) for f in fs]
        return sorted(out)

    def test_plan_changes_nothing(self):
        out, calls = self.run_base()
        for what in ('would: apt-get install chrony (apt removes systemd-timesyncd: it conflicts with chrony)',
                     'would: stop and disable systemd-timesyncd', 'would: enable and start chrony',
                     'would: make %s/swapfile (16 MB' % self.root, 'would: write %s/etc/systemd/system/swapfile.swap' % self.root,
                     'would: write %s/etc/systemd/journald.conf.d/deskd.conf' % self.root,
                     'would: apt-get install unattended-upgrades',
                     'would: write %s/etc/apt/apt.conf.d/52deskd-unattended-upgrades' % self.root, 'base done (plan)'):
            self.assertIn(what, out)
        self.assertEqual(self.files(), [])
        self.assertEqual([c for c in calls if c.startswith(MUTATING) or (c.startswith('apt-get') and ' -s ' not in c)], [])

    def test_apply_once_then_nothing(self):
        out, calls = self.run_base('--apply')
        self.assertIn('base done (apply)', out)
        did = '\n'.join(calls)
        for c in ('apt-get -y -o Dpkg::Options::=--force-confold install chrony', 'systemctl disable --now systemd-timesyncd',
                  'systemctl enable --now chrony', 'fallocate -l 16M %s/swapfile' % self.root, 'mkswap',
                  'systemctl enable --now swapfile.swap', 'systemctl restart systemd-journald',
                  'apt-get -y -o Dpkg::Options::=--force-confold --no-remove install unattended-upgrades'):
            self.assertIn(c, did)
        # timesyncd went off only after chrony was in
        self.assertLess(did.index('install chrony'), did.index('disable --now systemd-timesyncd'))
        self.assertEqual(self.read('etc/systemd/journald.conf.d/deskd.conf').splitlines()[-2:], ['[Journal]', 'SystemMaxUse=1G'])
        uu = self.read('etc/apt/apt.conf.d/52deskd-unattended-upgrades')
        self.assertIn('Unattended-Upgrade::Automatic-Reboot "false";', uu)
        self.assertIn('#clear Unattended-Upgrade::Allowed-Origins;', uu)
        self.assertIn('"${distro_id}:${distro_codename}-security";', uu)
        self.assertNotIn('-updates', uu)
        self.assertIn('What=/swapfile', self.read('etc/systemd/system/swapfile.swap'))
        self.assertEqual(self.read('etc/apt/apt.conf.d/20auto-upgrades'),
                         'APT::Periodic::Update-Package-Lists "1";\nAPT::Periodic::Unattended-Upgrade "1";\n')
        self.assertEqual(oct(os.stat(self.path('swapfile')).st_mode & 0o777), '0o600')
        before = {f: self.read(f) for f in self.files()}
        out, calls = self.run_base('--apply')
        self.assertEqual([c for c in calls if c.startswith(MUTATING) or (c.startswith('apt-get') and ' -s ' not in c)], [])
        self.assertIn('ok: chrony enabled and running', out)
        self.assertIn('ok: swap is on: /swapfile 16M', out)
        self.assertIn('ok: %s/etc/systemd/journald.conf.d/deskd.conf is current' % self.root, out)
        self.assertEqual({f: self.read(f) for f in self.files()}, before)

    def test_your_files_are_kept(self):
        os.makedirs(self.path('etc/systemd/journald.conf.d'))
        with open(self.path('etc/systemd/journald.conf.d/deskd.conf'), 'w') as f:
            f.write('[Journal]\nSystemMaxUse=4G\n')
        os.makedirs(self.path('etc/apt/apt.conf.d'))
        with open(self.path('etc/apt/apt.conf.d/20auto-upgrades'), 'w') as f:
            f.write('APT::Periodic::Unattended-Upgrade "0";\n')
        self.write_state('swaps', '/dev/sdb2 partition 4G 0B -2\n')   # a swap of yours: no swap file
        out, calls = self.run_base('--apply')
        self.assertEqual(self.read('etc/systemd/journald.conf.d/deskd.conf'), '[Journal]\nSystemMaxUse=4G\n')
        self.assertIn('SystemMaxUse=1G', self.read('etc/systemd/journald.conf.d/deskd.conf.new'))
        self.assertEqual(self.read('etc/apt/apt.conf.d/20auto-upgrades'), 'APT::Periodic::Unattended-Upgrade "0";\n')
        self.assertIn('differs: %s/etc/apt/apt.conf.d/20auto-upgrades' % self.root, out)
        self.assertNotIn('systemctl restart systemd-journald', '\n'.join(calls))   # nothing of ours took effect
        self.assertIn('ok: swap is on: /dev/sdb2 4G', out)
        self.assertFalse(os.path.exists(self.path('swapfile')))
        self.assertFalse(os.path.exists(self.path('etc/systemd/system/swapfile.swap')))

    def test_chrony_refused_if_apt_would_remove_more(self):
        self.write_state('apt-sim', 'Remv systemd-timesyncd [255]\nRemv ubuntu-minimal [1.539]\n')
        with open(self.path('swapfile'), 'w') as f:
            f.write('not swap')                       # a file of yours where the swap file would go
        out, calls = self.run_base('--apply')
        self.assertIn('REFUSED: installing chrony would also remove: ubuntu-minimal', out)
        self.assertIn('REFUSED: %s/swapfile exists and is not swap space' % self.root, out)
        did = '\n'.join(calls)
        self.assertNotIn('install chrony', did.replace('-s install chrony', ''))
        self.assertNotIn('disable --now systemd-timesyncd', did)   # the clock is never left without NTP
        self.assertNotIn('mkswap', did)
        self.assertTrue(os.path.exists(os.path.join(self.state, 'on-systemd-timesyncd')))
        self.assertEqual(slurp(self.path('swapfile')), 'not swap')

    def test_setup_runs_it_in_its_own_mode(self):
        text = slurp(os.path.join(DEPLOY, 'setup.sh'))
        self.assertIn('if [ "$MODE" = apply ]; then bash "$SRC/recorder/deploy/base.sh" --apply; '
                      'else bash "$SRC/recorder/deploy/base.sh"; fi', text)
        for script in ('setup.sh', 'base.sh', 'deploy.sh'):
            p = subprocess.run(['bash', '-n', os.path.join(DEPLOY, script)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == '__main__':
    unittest.main()
