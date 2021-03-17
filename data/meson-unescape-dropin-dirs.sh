#!/bin/sh
set -eu

unitdir="$1"

dir="${DESTDIR:-}${unitdir}"

for dropindir in "$dir"/*%*.d; do
  new="`echo "$dropindir" | tr % '\\\\'`"
  mkdir -p "$new"
  mv "$dropindir/"* "$new"
  rmdir "$dropindir"
done
