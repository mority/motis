#!/bin/bash
# arriveBy=true A/B: nigiri master a614f550 vs eda0261a (both coverage checks).
#   C  2026-09-01  on the delay date    -> no check may fire (regression check)
#   D  2026-08-15  before the coverage  -> both checks must fire
#   E  2026-10-01  after the coverage   -> RAPTOR fires, PONG cannot (known gap)
export WORK=$HOME/rt-coverage-bench
export DATA=/home/mority/data/germany/data
export NT=8
cd "$WORK"
for scen in C D E; do
  for alg in raptor pong; do
    for v in master pong-coverage; do
      echo "=== $scen / $alg / $v === start $(date -u +%T)"
      "$WORK/motis-nigiri-$v" batch \
          -d "$DATA" \
          -q "queries_${scen}_${alg}.txt" \
          -r "resp_${scen}_${alg}_${v}.txt" \
          --n_threads "$NT" \
          --random_delays --rd_date 2026-09-01 \
          > "batch_${scen}_${alg}_${v}.log" 2>&1
      echo "exit=$? end $(date -u +%T)  $(df -h /home | tail -1 | awk '{print $4}') free"
    done
  done
done
echo "ALL DONE $(date -u +%T)"
