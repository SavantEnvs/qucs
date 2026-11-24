#!/usr/bin/env bash
# mayhem/test.sh — upstream's golden-output suite (tests/ref): for every tests/<prj>/<file>.{sch,vs}
# that has references, run the normally built `qucs.real` CLI exactly as upstream's tests do
# (`-n` netlist, `-d -t sch|vs` dump) and diff the output against tests/ref/<prj>_<file>.<op>.
# Built by mayhem/build.sh (mayhem/build-test); this script only runs it.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SRC:=/mayhem}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

QUCS="$SRC/mayhem/build-test/qucs/qucs.real"
if [ ! -x "$QUCS" ]; then
  echo "test.sh: $QUCS missing — run mayhem/build.sh first" >&2
  emit_ctrf qucs-golden 0 1
  exit 1
fi
export QT_QPA_PLATFORM=offscreen QUCS_LIBRARY="$SRC/qucs-lib/library"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/qucstest.XXXXXX")"
export HOME="$WORK/home"; mkdir -p "$HOME"
trap 'rm -rf "$WORK"' EXIT

pass=0; fail=0
for ref in tests/ref/*; do
  base="$(basename "$ref")"            # <prj>_<file>.<in-ext>.<op>
  prj="${base%%_*}"; rest="${base#*_}"
  op="${rest##*.}"; infile="${rest%.*}"
  src="tests/$prj/$infile"
  [ -f "$src" ] || continue
  out="$WORK/$base"
  case "$op" in
    net) (cd "tests/$prj" && timeout 60 "$QUCS" -n -i "$infile" -o "$out") >/dev/null 2>&1 ;;
    sch|vs) (cd "tests/$prj" && timeout 60 "$QUCS" -d -i "$infile" -o "$out" -t "$op") >/dev/null 2>&1 ;;
    *) continue ;;
  esac
  # the references carry a bare "# Qucs" header; the tool writes "# Qucs <version> <path>"
  [ -f "$out" ] && sed -i '1s|^# Qucs .*|# Qucs|' "$out"
  # a few references also captured the tool's stderr "Warning: ..." lines; compare without them
  if [ -f "$out" ] && diff -q -B <(grep -v '^Warning: ' "$ref") <(grep -v '^Warning: ' "$out") >/dev/null 2>&1; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL: $base"
  fi
done
emit_ctrf qucs-golden "$pass" "$fail"
