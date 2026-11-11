#!/usr/bin/env bash
set -euo pipefail
cli=$(realpath "${1:-build/release/src/laminar}")
mkdir -p build/demo
root=$(mktemp -d build/demo/run.XXXXXX)
finish() {
  result=$?
  if (( result == 0 )); then
    rm -r -- "$root"
  else
    printf 'Demo failed; retained %s\n' "$root" >&2
  fi
}
trap finish EXIT
db="$root/db"
"$cli" put "$db" 6100 00ff
test "$("$cli" get "$db" 6100)" = 00ff
printf 'put\t62\tbeef\nput\t6100\t\r\ndelete\t62\n' > "$root/batch.tsv"
"$cli" batch "$db" "$root/batch.tsv"
test -z "$("$cli" get "$db" 6100)"
if "$cli" get "$db" 62; then
  exit 1
else
  test "$?" = 2
fi
printf 'Each CLI command reopens the database. Before compaction:\n'
before=$(sha256sum "$db/data.log")
"$cli" inspect "$db"
test "$(sha256sum "$db/data.log")" = "$before"
"$cli" compact "$db"
printf 'After compaction (record_count excludes baseline rows):\n'
"$cli" inspect "$db"
test -z "$("$cli" get "$db" 6100)"
"$cli" delete "$db" 6100
"$cli" compact "$db"
printf 'All deleted; the on-disk sequence still survives:\n'
"$cli" inspect "$db"
