#!/usr/bin/env bash
# Run the map text-index size benchmark (issue #110676).
# Usage:
#   ./bench.sh [clickhouse-binary] [data-path]
# Defaults:
#   binary: clickhouse (must be on PATH)
#   data:   tmp/bench110676  (relative to repo root)

set -euo pipefail

BINARY="${1:-clickhouse}"
DATADIR="${2:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)/tmp/bench110676}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$DATADIR/bench.log"

mkdir -p "$DATADIR"

echo "Running benchmark with binary: $BINARY"
echo "Data directory: $DATADIR"
echo "Log file: $LOG"
echo ""

"$BINARY" local \
    --path "$DATADIR" \
    --multiquery \
    --queries-file "$SCRIPT_DIR/bench.sql" \
    2>"$LOG"

echo "Done. Log at: $LOG"
