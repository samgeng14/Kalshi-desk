#!/usr/bin/env python3
"""Train the women's college basketball win-probability model (docs/wcbb_model.md, plan 5(f)).

tools/train_bball.py --league wcbb, under the name the README, the handoff and ctest train-wcbb
use: the same flags (--panel, --features, --held-out, --out, --version, --force, --self-test),
the same defaults (features logit_p0,margin_over_sd,logit_p0_x_tau,possession; --out
models/wcbb_logit_v1.json) and the same checks. The panel, the features, the fit, the baselines
and the file are documented in train_bball.py; --self-test runs its wcbb checks.

Usage:
  python3 tools/train_wcbb.py --panel wbpanel.csv
  python3 tools/train_wcbb.py --panel wbpanel.csv --held-out 2024-25 \\
      --features logit_p0,margin_over_sd,logit_p0_x_tau,possession,possession_over_sd \\
      --out models/wcbb_logit_v2.json
  python3 tools/train_wcbb.py --self-test
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_bball  # noqa: E402


def run(argv):
    return train_bball.run(argv, league="wcbb", prog="train_wcbb", description=__doc__.split("\n")[0])


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
