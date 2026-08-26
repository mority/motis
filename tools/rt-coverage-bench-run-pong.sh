#!/bin/bash
# A/B for nigiri "pong: drop rtt ptr if not needed" (eda0261a) vs nigiri master (a614f550).
# Same OD pairs / dates as the range-RAPTOR run, only algorithm=PONG.
export WORK=$HOME/rt-coverage-bench
export DATA=/home/mority/data/germany/data
export DELAY_DATE=2026-09-01
export NT=8
cd "$WORK"
for scen in A B; do
  for v in master pong-coverage; do
    echo "=== $scen / $v === start $(date -u +%T)"
    "$WORK/motis-nigiri-$v" batch \
        -d "$DATA" \
        -q "queries_${scen}_pong.txt" \
        -r "resp_${scen}_pong_${v}.txt" \
        --n_threads "$NT" \
        --random_delays --rd_date "$DELAY_DATE" \
        > "batch_${scen}_pong_${v}.log" 2>&1
    echo "exit=$? end $(date -u +%T)"
    df -h /home | tail -1
  done
done
echo "ALL DONE $(date -u +%T)"
