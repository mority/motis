#!/usr/bin/env python3
"""Counter comparison for the PONG rt-coverage A/B (see rt-coverage-bench.txt)."""
import json, os, sys

work = os.environ.get("WORK", os.path.expanduser("~/rt-coverage-bench"))
prefix = sys.argv[1] if len(sys.argv) > 1 else "resp"
suffix = "_pong_"

def load(p):
    with open(os.path.join(work, p)) as f:
        return [json.loads(l) for l in f]

metrics = ["execute_time", "n_routes_visited", "n_earliest_trip_calls",
           "n_footpaths_visited", "n_earliest_arrival_updated_by_route",
           "n_earliest_arrival_updated_by_footpath", "lb_time",
           "n_execute_fwd", "n_execute_bwd", "interval_extensions"]

for scen, title in [("A", "A: queries on the delay date (check must NOT fire)"),
                    ("B", "B: queries a month later (check MUST fire)")]:
    a = load(f"{prefix}_{scen}{suffix}master.txt")
    b = load(f"{prefix}_{scen}{suffix}pong-coverage.txt")
    print(f"\n{title}   [{len(a)} queries]")
    print(f"  {'metric':42} {'master':>16} {'pong-coverage':>16} {'ratio':>9}")
    for m in metrics:
        sa = sum(r.get("debugOutput", {}).get(m, 0) for r in a)
        sb = sum(r.get("debugOutput", {}).get(m, 0) for r in b)
        r = f"{sa / sb:.2f}x" if sb else "-"
        print(f"  {m:42} {sa:>16,} {sb:>16,} {r:>9}")
    same = sum(1 for x, y in zip(a, b)
               if x.get("itineraries") == y.get("itineraries"))
    ia = sum(len(r.get("itineraries", [])) for r in a)
    ib = sum(len(r.get("itineraries", [])) for r in b)
    algs = {r.get("debugOutput", {}).get("algorithm") for r in a}
    print(f"  itineraries: master={ia}, pong-coverage={ib}; "
          f"identical: {same}/{len(a)}   algorithm ids used: {algs}")
