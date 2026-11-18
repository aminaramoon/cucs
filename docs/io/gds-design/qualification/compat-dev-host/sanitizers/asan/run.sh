#!/usr/bin/env bash
# usage: run.sh LOG [catch2 args...]
LOG=$1; shift
export ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:${ADM:+alloc_dealloc_mismatch=0:}halt_on_error=1${SUPP:+:suppressions=$PWD/tools/gds/sanitizer.supp} UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0
timeout 3000 ./build/asan/test/cucascade_io_tests "$@" > "$LOG" 2>&1
echo "rc=$?" >> "$LOG"
