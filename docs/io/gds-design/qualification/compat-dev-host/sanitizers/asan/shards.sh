#!/usr/bin/env bash
# usage: shards.sh N  (runs every shard in a fresh process, declaration order, one retry for Catch redirect abort)
N=$1; shift
for ((i=0;i<N;i++)); do
  for attempt in 1 2 3; do
    ADM=${ADM:-} SUPP=${SUPP:-} bash /tmp/qual/asan/run.sh /tmp/qual/asan/shard-$i.log --order decl --shard-count $N --shard-index $i "$@"
    if grep -q "redirect is already active" /tmp/qual/asan/shard-$i.log; then echo "shard $i attempt $attempt: catch-redirect abort"; continue; fi
    break
  done
  echo "shard $i: $(grep -h 'test cases:' /tmp/qual/asan/shard-$i.log) $(tail -1 /tmp/qual/asan/shard-$i.log)"
done
