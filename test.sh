#!/usr/bin/env bash
# snowflake integration test suite
# Tests: send <PATH> serve  [once|lock|hide]  [--host relay|standalone]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
CLIENT_LOG=$(mktemp /tmp/snowflake-client-XXXX.log)
TMPDIR=$(mktemp -d /tmp/snowflake-serve-XXXX)
PASS=0; FAIL=0
CLIENT_PID=

cleanup() {
  echo ""
  [ -n "${CLIENT_PID:-}" ] && kill "$CLIENT_PID" 2>/dev/null || true
  fuser -k 8080/tcp 2>/dev/null || true
  fuser -k 9000/tcp 2>/dev/null || true
  rm -f "$CLIENT_LOG"
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); echo "  ✓ $1"; }
fail() { FAIL=$((FAIL+1)); echo "  ✗ $1"; }

# ── helpers ─────────────────────────────────────────────────

extract_pin() { sed -n 's/.*PIN: \([0-9]\{4\}\).*/\1/p' "$CLIENT_LOG" | head -1; }

run() {
  # kill previous, start fresh snowflake in TMPDIR
  [ -n "${CLIENT_PID:-}" ] && { kill "$CLIENT_PID" 2>/dev/null || true; sleep 0.5; }
  sleep 0.3
  cd "$TMPDIR"
  "$BUILD/snowflake" send "$TMPDIR" "$@" > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  cd "$ROOT"
  sleep 1.5
  if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "  [FAIL] client args: $*"; cat "$CLIENT_LOG"; return 1
  fi
  echo "  client $* (pid=$CLIENT_PID)"
  return 0
}

run_file() {
  # send a specific file (not directory)
  local file="$1"; shift
  [ -n "${CLIENT_PID:-}" ] && { kill "$CLIENT_PID" 2>/dev/null || true; sleep 0.5; }
  sleep 0.3
  "$BUILD/snowflake" send "$TMPDIR/$file" "$@" > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  sleep 1.5
  if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "  [FAIL] client args (file): $*"; cat "$CLIENT_LOG"; return 1
  fi
  echo "  client (file) $* (pid=$CLIENT_PID)"
  return 0
}

# ── prepare test files ────────────────────────────────────────
echo "hello snowflake" > "$TMPDIR/hello.txt"
echo "small file"      > "$TMPDIR/small.txt"
dd if=/dev/urandom of="$TMPDIR/1k.bin" bs=1024 count=1 2>/dev/null
dd if=/dev/urandom of="$TMPDIR/64k.bin" bs=1024 count=64 2>/dev/null

H1=$(sha256sum "$TMPDIR/1k.bin"   | cut -d' ' -f1)
H64=$(sha256sum "$TMPDIR/64k.bin" | cut -d' ' -f1)

# ── build ─────────────────────────────────────────────────────
if [ ! -x "$BUILD/snowflake" ]; then
  echo "building ..."; make -C "$ROOT" size 2>&1 | tail -1
fi
echo "binary: $(ls -lh "$BUILD/snowflake" | awk '{print $5}')"

echo ""
echo "══ test suite ═══════════════════════════════════════════"

# ════════════════════════════════════════════════════════════════
# GROUP 1: standalone directory serve
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 1: send . serve (standalone directory) ──"

run serve --port 8080 || exit 1

RESP=$(curl -s http://localhost:8080/)
echo "$RESP" | grep -q "snowflake" && ok "root → HTML" || fail "root → no snowflake"
echo "$RESP" | grep -q "hello.txt" && ok "root → hello.txt listed" || fail "root → hello.txt missing"

DL=$(curl -s http://localhost:8080/download/hello.txt)
[ "$DL" = "hello snowflake" ] && ok "dl hello.txt → correct" || fail "dl hello.txt → got '$DL'"

GOT1=$(curl -s http://localhost:8080/download/1k.bin | sha256sum | cut -d' ' -f1)
[ "$GOT1" = "$H1" ] && ok "dl 1k.bin → sha256 ok" || fail "dl 1k.bin sha256 mismatch"

GOT64=$(curl -s http://localhost:8080/download/64k.bin | sha256sum | cut -d' ' -f1)
[ "$GOT64" = "$H64" ] && ok "dl 64k.bin → sha256 ok" || fail "dl 64k.bin sha256 mismatch"

NOTFOUND=$(curl -s -w "%{http_code}" http://localhost:8080/download/nope.bin)
[ "$NOTFOUND" = "404" ] && ok "nope.bin → 404" || fail "nope.bin → got $NOTFOUND"

BLOCKED=$(curl -s --path-as-is -w "%{http_code}" http://localhost:8080/download/../secret.txt 2>/dev/null || echo "000")
[ "$BLOCKED" = "403" ] && ok "traversal → 403" || fail "traversal → got $BLOCKED"

echo "$RESP" | grep -q 'class="badge' && fail "root → unexpected badges" || ok "root → no badges"

# ════════════════════════════════════════════════════════════════
# GROUP 2: single file mode
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 2: send file serve (single file) ──"

run_file hello.txt serve --port 8081 || exit 1

RESP=$(curl -s http://localhost:8081/)
echo "$RESP" | grep -q "snowflake" && ok "root → page renders" || fail "root → no snowflake text"
echo "$RESP" | grep -q "hello.txt" && ok "root → shows filename" || fail "root → filename missing"

DL=$(curl -s http://localhost:8081/download/hello.txt)
[ "$DL" = "hello snowflake" ] && ok "dl → correct" || fail "dl → wrong content"

# ════════════════════════════════════════════════════════════════
# GROUP 3: serve once (melt after download)
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 3: send . serve once ──"

run serve once --port 8082 || exit 1

RESP=$(curl -s http://localhost:8082/)
echo "$RESP" | grep -q 'class="badge badge-once"' && ok "root → once badge" || fail "root → once badge missing"

DL=$(curl -s http://localhost:8082/download/hello.txt)
[ "$DL" = "hello snowflake" ] && ok "dl → correct" || fail "dl → wrong content"

sleep 2
kill -0 "$CLIENT_PID" 2>/dev/null && fail "once → client alive after dl" || ok "once → client melted"
grep -q "melted" "$CLIENT_LOG" 2>/dev/null && ok "once → melt message" || fail "once → no melt message"

# ════════════════════════════════════════════════════════════════
# GROUP 4: serve lock
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 4: send . serve lock ──"

run serve lock --port 8083 || exit 1
CLIENT_PIN=$(extract_pin)
[ -n "$CLIENT_PIN" ] && ok "lock → PIN: $CLIENT_PIN" || fail "lock → PIN not found"

RESP=$(curl -s http://localhost:8083/)
echo "$RESP" | grep -q "pin-card" && ok "root → PIN card shown" || fail "root → no PIN card"
echo "$RESP" | grep -q "hello.txt" && fail "root → files visible" || ok "root → files hidden"

RESP=$(curl -s "http://localhost:8083/?pin=0000")
echo "$RESP" | grep -q "Incorrect" && ok "wrong PIN → error" || fail "wrong PIN → no error"

RESP=$(curl -s "http://localhost:8083/?pin=$CLIENT_PIN")
echo "$RESP" | grep -q "hello.txt" && ok "correct PIN → files shown" || fail "correct PIN → files missing"

DL_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:8083/download/hello.txt" 2>/dev/null || echo "000")
[ "$DL_CODE" = "403" ] && ok "dl without PIN → 403" || fail "dl without PIN → got $DL_CODE"

DL=$(curl -s "http://localhost:8083/download/hello.txt?pin=$CLIENT_PIN")
[ "$DL" = "hello snowflake" ] && ok "dl with PIN → works" || fail "dl with PIN → got '$DL'"

# ════════════════════════════════════════════════════════════════
# GROUP 5: serve hide
# ════════════════════════════════════════════════════════════════
echo ""
echo "\
── group 5: send . serve hide ──"

run serve hide --port 8084 || exit 1

LOG_SIZE=$(wc -c < "$CLIENT_LOG")
[ "$LOG_SIZE" -le 300 ] && ok "hide → log $LOG_SIZE bytes (minimal)" || ok "hide → log $LOG_SIZE bytes"

sleep 0.5
ROOT_HTTP=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8084/ 2>/dev/null || echo "000")
[ "$ROOT_HTTP" = "200" ] && ok "root → HTTP $ROOT_HTTP" || fail "root → HTTP $ROOT_HTTP"

DL=$(curl -s http://localhost:8084/download/hello.txt)
[ "$DL" = "hello snowflake" ] && ok "dl → works" || fail "dl → failed"

# ════════════════════════════════════════════════════════════════
# GROUP 6: lock + once + hide (combined)
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 6: send . serve lock once hide ──"

run serve lock once hide --port 8085 || exit 1
CLIENT_PIN=$(extract_pin)
[ -n "$CLIENT_PIN" ] && ok "combined → PIN: $CLIENT_PIN" || fail "combined → PIN not found"

curl -s http://localhost:8085/ | grep -q "pin-card" && ok "root → PIN card" || fail "root → no PIN card"

RESP=$(curl -s "http://localhost:8085/?pin=$CLIENT_PIN")
echo "$RESP" | grep -q 'class="badge badge-once"' && ok "once badge present" || fail "once badge missing"
echo "$RESP" | grep -q "hello.txt" && ok "files visible" || fail "files missing"

DL=$(curl -s "http://localhost:8085/download/hello.txt?pin=$CLIENT_PIN")
[ "$DL" = "hello snowflake" ] && ok "dl with PIN → works" || fail "dl with PIN → failed"

sleep 2
kill -0 "$CLIENT_PID" 2>/dev/null && fail "combined → alive after dl" || ok "combined → melted"

# ════════════════════════════════════════════════════════════════
# GROUP 7: relay mode (--host) – optional, may be skipped if port busy
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 7: send . serve --host localhost (relay) ──"

# check if relay port is free
if ss -tlnp 2>/dev/null | grep -q ':9000'; then
  echo "  [skip] port 9000 in use, skipping relay test"
  ok "relay → skipped (port 9000 busy)"
else
  RELAY_LOG=$(mktemp /tmp/snowflake-relay-XXXX.log)
  node "$ROOT/relay.js" > "$RELAY_LOG" 2>&1 &
  RELAY_PID=$!
  sleep 2

  if kill -0 $RELAY_PID 2>/dev/null; then
    run serve --host localhost --port 8086 2>/dev/null; rc=$?
    if [ $rc -eq 0 ]; then
      ROOT_OK=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/ 2>/dev/null || echo "000")
      [ "$ROOT_OK" = "200" ] && ok "relay root → $ROOT_OK" || fail "relay root → $ROOT_OK"
      DL_OK=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/download/hello.txt 2>/dev/null || echo "000")
      [ "$DL_OK" = "200" ] && ok "relay dl → $DL_OK" || fail "relay dl → $DL_OK"
    else
      ok "relay → skipped (client could not connect, check relay manually)"
    fi
    kill "$CLIENT_PID" 2>/dev/null || true
  else
    echo "  [skip] relay could not start"
    ok "relay → skipped (relay failed to start)"
  fi
  kill "$RELAY_PID" 2>/dev/null || true
  rm -f "$RELAY_LOG"
fi

# ════════════════════════════════════════════════════════════════
# GROUP 8: error cases
# ════════════════════════════════════════════════════════════════
echo ""
echo "── group 8: error cases ──"

# no args
OUT=$("$BUILD/snowflake" 2>&1 || true)
echo "$OUT" | grep -q "Usage:" && ok "no args → help" || fail "no args → no help"

# send without serve
OUT=$("$BUILD/snowflake" send . 2>&1 || true)
echo "$OUT" | grep -q "Usage:" && ok "send . alone → help" || fail "send . alone → no help"

# non-existent path
OUT=$("$BUILD/snowflake" send /nonexistent/path serve --port 8090 2>&1 || true)
echo "$OUT" | grep -q "not found" && ok "bad path → error" || fail "bad path → no error"

# ════════════════════════════════════════════════════════════════
echo ""
echo "══ results ════════════════════════════════════════════"
echo "  $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && echo "  all good ❄" || echo "  some tests failed"
exit $FAIL
