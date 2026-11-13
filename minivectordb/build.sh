#!/bin/sh
# Builds the MiniVectorDB server. The project has no third party dependencies,
# so a C++17 compiler and the standard library are all that is required.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/build/minivectordb"}
mkdir -p "$(dirname "$out")"

CXX=${CXX:-}
if [ -z "$CXX" ]; then
  if command -v g++-13 >/dev/null 2>&1; then
    CXX=g++-13
  else
    CXX=g++
  fi
fi
FLAGS=${FLAGS:--O2 -std=c++17 -Wall -Wextra -fno-plt}

# shellcheck disable=SC2086
"$CXX" $FLAGS -o "$out" \
  "$root/minivectordb/src/core.cpp" \
  "$root/minivectordb/src/sql.cpp" \
  "$root/minivectordb/src/storage.cpp" \
  "$root/minivectordb/src/table.cpp" \
  "$root/minivectordb/src/hnsw.cpp" \
  "$root/minivectordb/src/db.cpp" \
  "$root/minivectordb/src/main.cpp"

echo "built $out"
