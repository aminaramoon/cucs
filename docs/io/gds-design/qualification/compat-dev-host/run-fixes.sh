#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Only the engine measurements invalidated by R5/R8/R9; serial, compat diagnostic only.
set -euo pipefail
OUT=${1:?fresh results directory}
SCRATCH=${2:?fresh scratch directory}
mkdir -p "$OUT" "$SCRATCH"
if [[ ! -e $OUT/manifest.tsv ]]; then
  printf 'step\tmount\tfs_class\tdevice\tqd\top_mib\tdirection\tvariant\tpath\texit_code\n' >"$OUT/manifest.tsv"
fi
common=(--engine async --require-direct-evidence 0 --override mount_prefix:/ --override fs_type:ext4
  --slow-floor-us 2000 --stuck-ms 20 --size-mib 1024 --task-mib 256 --single-mib 256
  --tasks 4 --qd 32 --arena-mib 256 --drop-cache --verify 1 --min-seconds 5 --repetitions 3)
for step in B-3 B-4 B-5 B-6; do
  workload=sustained; direction=read; extra=(); ops=(4)
  case $step in
    B-3) ops=(1 2 4 8) ;;
    B-4) workload=restore ;;
    B-5) workload=single ;;
    B-6) workload=spill; direction=write; extra=(--flush 1) ;;
  esac
  for op in "${ops[@]}"; do
    for threads in 1 4; do
      stem="fix-$step-t$threads-op$op"
      [[ ! -e $OUT/$stem.csv ]] || continue # Resume without repeating valid measurements.
      if [[ -e $OUT/$stem.log ]]; then mv -- "$OUT/$stem.log" "$OUT/$stem.previous-$(date +%s).log"; fi
      create=(); [[ -e $SCRATCH/$stem.bin ]] || create=(--create-file)
      cmd=(./build/release/cucascade_cufile_engine_bench --file "$SCRATCH/$stem.bin" "${create[@]}"
        --workload "$workload" "${common[@]}" --threads "$threads" --op-mib "$op" "${extra[@]}" --csv "$OUT/$stem.csv")
      printf '%q ' "${cmd[@]}" >>"$OUT/commands.log"; printf '\n' >>"$OUT/commands.log"
      rc=0; timeout --kill-after=60 180 "${cmd[@]}" >"$OUT/$stem.log" 2>&1 || rc=$?
      printf '%s\tlvm\tcompat\t0\t32\t%s\t%s\tregistered\t%s.log\t%s\n' "$step" "$op" "$direction" "$stem" "$rc" >>"$OUT/manifest.tsv"
      ((rc == 0)) || echo "FAILED $stem rc=$rc" >>"$OUT/failures.log"
    done
  done
done
for workload in restore spill; do
  extra=(); [[ $workload != spill ]] || extra=(--flush 1)
  timeout --kill-after=60 180 ./build/release/cucascade_cufile_engine_bench "${common[@]}" \
    --file "$SCRATCH/soak-$workload.bin" --create-file --workload "$workload" \
    --size-mib 256 --task-mib 64 --single-mib 64 --threads 2 --op-mib 4 "${extra[@]}" \
    --soak-minutes 0.5 --recreate-every-s 10 --sample-every-s 2 --csv "$OUT/soak-$workload.csv" \
    >"$OUT/soak-$workload.log" 2>&1
done
python tools/gds/analyze_gates.py "$OUT"
