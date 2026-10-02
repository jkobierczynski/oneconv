#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
# Regression run over public OneNote sample files (not redistributed here).
#   tests/run_samples.sh [path/to/oneconv]
# Fetches the sample corpora of onenote.rs and Apache Tika, converts every
# section to Markdown, HTML and ENEX, validates the ENEX files, and reports failures.
set -u
BIN="${1:-build/oneconv}"
WORK="${WORK:-/tmp/oneconv-samples}"
mkdir -p "$WORK"
rm -rf "$WORK/out" "$WORK/enex"
if [ ! -d "$WORK/onenote.rs" ]; then
  git clone --depth 1 https://github.com/msiemens/onenote.rs "$WORK/onenote.rs"
fi
if [ ! -d "$WORK/tika" ]; then
  git clone --depth 1 --filter=blob:none --no-checkout https://github.com/apache/tika "$WORK/tika"
  P=tika-parsers/tika-parsers-standard/tika-parsers-standard-modules/tika-parser-microsoft-module/src/test/resources/test-documents
  git -C "$WORK/tika" sparse-checkout set --no-cone "$P/*.one"
  git -C "$WORK/tika" checkout
fi
fail=0; total=0
while IFS= read -r -d '' f; do
  total=$((total+1))
  out="$WORK/out/$total"
  if ! "$BIN" -q -f both -o "$out" "$f" >/dev/null 2>"$WORK/err.txt"; then
    case "$f" in *fuzz*) ;; *) fail=$((fail+1)); echo "FAIL: $f"; head -3 "$WORK/err.txt";; esac
  fi
  "$BIN" -q -f enex -o "$WORK/enex/$total" "$f" >/dev/null 2>&1 || true
done < <(find "$WORK/onenote.rs/crates/parser/tests/samples" "$WORK/tika" -name '*.one' -size +200c -print0)
echo "$total sections converted, $fail failures"
if command -v python3 >/dev/null; then
  python3 "$(dirname "$0")/check_enex.py" "$WORK/enex" || fail=$((fail+1))
fi
[ "$fail" -eq 0 ]
