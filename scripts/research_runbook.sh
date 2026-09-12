#!/usr/bin/env bash
# The research pipeline, in order, as one command. Pasting a commented list into a shell hands the
# comments to argparse as arguments, which is what happened last time; this exists so that cannot
# happen again.
#
#   ./scripts/research_runbook.sh probe      validate the endpoints against production first
#   ./scripts/research_runbook.sh record     start the tick recorder (runs until you stop it)
#   ./scripts/research_runbook.sh backfill   pull history (minutes, not weeks)
#   ./scripts/research_runbook.sh audit      grade what actually arrived, per contract
#   ./scripts/research_runbook.sh alpha      univariate alpha decay, before any model
#   ./scripts/research_runbook.sh all        probe -> backfill -> audit -> alpha
set -euo pipefail
cd "$(dirname "$0")/.."
DAYS=${DAYS:-90}

probe()    { echo "== validating endpoints against production =="; python3 scripts/perps_api_probe.py; }
backfill() { echo "== backfilling ${DAYS}d of history =="; python3 scripts/backfill_perps_history.py --days "$DAYS"; }
audit()    { echo "== auditing what arrived =="; python3 scripts/perps_audit.py; }
alpha()    { echo "== univariate alpha decay =="; python3 scripts/alpha_decay.py; }
record()   {
  echo "== recorder: leave this running. Ctrl-C to stop. =="
  echo "   It is the only source of contemporaneous spread, queue and imbalance data,"
  echo "   and it is gated on wall-clock time -- an hour not recorded cannot be bought back."
  exec python3 scripts/perps_recorder.py
}

case "${1:-all}" in
  probe)    probe ;;
  record)   record ;;
  backfill) backfill ;;
  audit)    audit ;;
  alpha)    alpha ;;
  all)      probe; backfill; audit; alpha
            echo
            echo "Done. Read in this order:"
            echo "  research/margin_api_empirical_spec.md   what production actually returns"
            echo "  reports/perps_data_audit.md             which contracts are fittable"
            echo "  reports/alpha_decay.md                  whether any primitive signal exists"
            echo
            echo "Start the recorder too, in its own shell:  ./scripts/research_runbook.sh record" ;;
  *) echo "usage: $0 {probe|record|backfill|audit|alpha|all}"; exit 2 ;;
esac
