#!/usr/bin/env bash
# Step 3 (compat-dev-host): B-1, B-3/B-4/B-5/B-6/B-9 with moderate sizes, async-with-override vs legacy.
# Mirrors tools/gds/qualify.sh's matrix (same flags/manifest format) but with sizes small enough for /tmp.
# Usage (repo root): pixi run bash docs/io/gds-design/qualification/compat-dev-host/run-step3.sh OUTDIR SCRATCHDIR
set -u
OUT=$(realpath -m "$1"); WORK=$(realpath -m "$2"); mkdir -p "$OUT/tmp" "$WORK"
export TMPDIR="$OUT/tmp" CUFILE_LOGFILE_PATH="$OUT/cufile.log"
QD_BIN=build/release/cucascade_cufile_async_qd; ENGINE_BIN=build/release/cucascade_cufile_engine_bench
label=lvm fs=ext4 dev=0 mountpoint=/
FILE_MIB=1024 TASK_MIB=256 SINGLE_MIB=256 QD=32 ARENA=256 SECS=5 REPS=3
run() { local step=$1 qd=$2 op=$3 dir=$4 variant=$5 path=$6; shift 6
  printf '%q ' "$@" >>"$OUT/commands.log"; echo >>"$OUT/commands.log"
  "$@" >"$OUT/$path" 2>"$OUT/$path.stderr"; local rc=$?
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$step" "$label" "$fs" "$dev" "$qd" "$op" "$dir" "$variant" "$path" "$rc" >>"$OUT/manifest.tsv"
  ((rc==0)) || echo "FAILED ($rc): $step $variant $path" | tee -a "$OUT/failures.log"; }
printf 'step\tmount\tfs_class\tdevice\tqd\top_mib\tdirection\tvariant\tpath\texit_code\n' >"$OUT/manifest.tsv"
run B-1 0 0 - micro "$label-d$dev-B-1.csv" "$QD_BIN" --microbench --device "$dev"
for step in B-3 B-4 B-5 B-6 B-9; do
  workload=sustained; case $step in B-4) workload=restore;; B-5) workload=single;; B-6) workload=spill;; esac
  variants=(legacy registered); [[ $step == B-9 ]] && variants=(legacy registered posix forced-compat)
  ops=(4); [[ $step == B-3 ]] && ops=(1 2 4 8)
  for op in "${ops[@]}"; do for variant in "${variants[@]}"; do for threads in 1 4; do
    engine=async; extra=(); policy=(--require-direct-evidence 0 --override "mount_prefix:$mountpoint" --override fs_type:ext4)
    [[ $variant != legacy && $variant != posix ]] || engine=legacy
    [[ $variant != posix ]] || extra+=(--posix-only)
    prefix=(env); [[ $variant != forced-compat ]] || prefix+=(CUFILE_FORCE_COMPAT_MODE=true)
    stem="$label-d$dev-$step-$variant-t$threads-op$op"
    run "$step" "$QD" "$op" "$([[ $step == B-6 ]] && echo write || echo read)" "$variant" "$stem.log" \
      "${prefix[@]}" "$ENGINE_BIN" --file "$WORK/$stem.bin" --create-file --workload "$workload" --engine "$engine" \
      --size-mib "$FILE_MIB" --device "$dev" --tasks 4 --task-mib "$TASK_MIB" --single-mib "$SINGLE_MIB" --drop-cache --verify 1 \
      --min-seconds "$SECS" --repetitions "$REPS" --qd "$QD" --arena-mib "$ARENA" --op-mib "$op" "${policy[@]}" --threads "$threads" "${extra[@]}" --csv "$OUT/$stem.csv"
    rm -f "$WORK/$stem.bin"*
  done; done; done
done
echo done >"$OUT/DONE"
