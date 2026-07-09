#!/bin/bash
#
# test.sh - Run monkey test + perfetto trace and collect all logs
#
# Usage: ./test.sh [options]
#
# Options:
#   -e  MONKEY_EVENTS    Number of monkey events (default: 600)
#   -t  PERFETTO_TIME    Perfetto trace duration in seconds (default: 120)
#   -p  PACKAGE          Target specific package (default: all packages)
#   -d  PERFETTO_DIR     Perfetto data sources (default: "sched freq idle_atom wakelock binder_driver")
#   -o  OUTPUT_DIR       Output directory (default: ./logs)
#   -h                   Show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/logs"
MONKEY_EVENTS=600
PERFETTO_TIME=120
TARGET_PACKAGE=""
PERFETTO_DSOURCES="sched freq idle_atom wakelock binder_driver"
DEVICE_TMP="/data/local/tmp"
NOW=""

usage() {
    sed -n '3,14p' "$0"
    exit 0
}

while getopts "e:t:p:d:o:h" opt; do
    case "$opt" in
        e) MONKEY_EVENTS="$OPTARG" ;;
        t) PERFETTO_TIME="$OPTARG" ;;
        p) TARGET_PACKAGE="$OPTARG" ;;
        d) PERFETTO_DSOURCES="$OPTARG" ;;
        o) OUTPUT_DIR="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

# Timestamp for this run
NOW=$(date +%Y%m%d_%H%M%S)
RUN_DIR="${OUTPUT_DIR}/run_${NOW}"
mkdir -p "$RUN_DIR"

echo "=== Monkey + Perfetto Log Collector ==="
echo "Output dir : $RUN_DIR"
echo "Events     : $MONKEY_EVENTS"
echo "Perfetto   : ${PERFETTO_TIME}s [${PERFETTO_DSOURCES}]"
echo "Package    : ${TARGET_PACKAGE:-all}"

# ------------------------------------------------------------------
# 1. Verify device
# ------------------------------------------------------------------
DEVICE=$(adb devices | awk 'NR==2{print $1}')
if [ -z "$DEVICE" ]; then
    echo "ERROR: No device connected via adb"
    exit 1
fi
echo "Device     : $DEVICE"

# ------------------------------------------------------------------
# 2. Clear old logs on device
# ------------------------------------------------------------------
echo ""
echo "[1/6] Clearing old logs on device..."
adb shell logcat -c 2>/dev/null || true
adb shell "rm -f ${DEVICE_TMP}/logcat.txt* ${DEVICE_TMP}/perfetto_trace*" 2>/dev/null || true

# ------------------------------------------------------------------
# 3. Start logcat recording (background)
# ------------------------------------------------------------------
echo "[2/6] Starting logcat recording..."
adb shell "nohup logcat -v threadtime -f ${DEVICE_TMP}/logcat.txt -r 10240 -n 3 >/dev/null 2>&1 &"
LOGCAT_PID=$(adb shell "pidof -s logcat" 2>/dev/null || echo "unknown")
echo "       logcat pid: $LOGCAT_PID"

# ------------------------------------------------------------------
# 4. Start perfetto trace (background)
# ------------------------------------------------------------------
echo "[3/6] Starting perfetto trace (${PERFETTO_TIME}s)..."
adb shell -t "nohup perfetto -o ${DEVICE_TMP}/perfetto_trace.perfetto-trace -t ${PERFETTO_TIME}s ${PERFETTO_DSOURCES} >/dev/null 2>&1 &"
PERFETTO_PID=$(adb shell "pidof -s perfetto" 2>/dev/null || echo "unknown")
echo "       perfetto pid: $PERFETTO_PID"

# ------------------------------------------------------------------
# 5. Run monkey test
# ------------------------------------------------------------------
MONKEY_ARGS="--throttle 100 --ignore-crashes --ignore-timeouts --ignore-security-exceptions --monitor-native-crashes -v -v -v"
if [ -n "$TARGET_PACKAGE" ]; then
    MONKEY_ARGS="$MONKEY_ARGS -p $TARGET_PACKAGE"
fi

echo "[4/6] Running monkey test (${MONKEY_EVENTS} events)..."
MONKEY_START=$(date +%s)
set +e
adb shell "monkey ${MONKEY_ARGS} ${MONKEY_EVENTS}" > "${RUN_DIR}/monkey_output.txt" 2>&1
MONKEY_EXIT=$?
MONKEY_END=$(date +%s)
MONKEY_DURATION=$((MONKEY_END - MONKEY_START))
echo "       monkey finished (exit=$MONKEY_EXIT, duration=${MONKEY_DURATION}s)"

# Check for crashes
if grep -qi "crashed\|anr\|native crash" "${RUN_DIR}/monkey_output.txt" 2>/dev/null; then
    echo "       WARNING: crashes/ANRs detected in monkey output"
fi

# ------------------------------------------------------------------
# 6. Stop perfetto & logcat, pull artifacts
# ------------------------------------------------------------------
echo "[5/6] Stopping perfetto and logcat..."
adb shell "killall -2 perfetto" 2>/dev/null || true
adb shell "killall -2 logcat" 2>/dev/null || true
sleep 2
adb shell "killall -9 logcat" 2>/dev/null || true  # force if needed

echo "[6/6] Pulling logs to $RUN_DIR..."
adb pull "${DEVICE_TMP}/logcat.txt"       "$RUN_DIR/" 2>/dev/null || echo "       (no logcat.txt)"
adb pull "${DEVICE_TMP}/logcat.txt.1"     "$RUN_DIR/" 2>/dev/null || echo "       (no logcat.txt.1)"
adb pull "${DEVICE_TMP}/logcat.txt.2"     "$RUN_DIR/" 2>/dev/null || echo "       (no logcat.txt.2)"
adb pull "${DEVICE_TMP}/logcat.txt.3"     "$RUN_DIR/" 2>/dev/null || echo "       (no logcat.txt.3)"
adb pull "${DEVICE_TMP}/perfetto_trace.perfetto-trace" "$RUN_DIR/" 2>/dev/null || echo "       (no perfetto trace)"

# Also grab a live logcat snapshot
adb shell logcat -d -v threadtime > "${RUN_DIR}/logcat_snapshot.txt" 2>/dev/null

# Grab tombstones
TOMBSTONE_COUNT=$(adb shell "ls /data/tombstones/tombstone_* 2>/dev/null" | wc -l)
if [ "$TOMBSTONE_COUNT" -gt 0 ]; then
    mkdir -p "${RUN_DIR}/tombstones"
    echo "       pulling ${TOMBSTONE_COUNT} tombstone(s)..."
    for ts in $(adb shell "ls /data/tombstones/tombstone_[0-9]* 2>/dev/null"); do
        adb pull "$ts" "${RUN_DIR}/tombstones/" 2>/dev/null || true
    done
fi

# Clean device temp files
adb shell "rm -f ${DEVICE_TMP}/logcat.txt* ${DEVICE_TMP}/perfetto_trace*" 2>/dev/null || true

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------
echo ""
echo "=== Summary ==="
echo "Run dir     : ${RUN_DIR}"
echo "Monkey exit : ${MONKEY_EXIT}"
echo ""
echo "Files:"
ls -lh "${RUN_DIR}/" 2>/dev/null | awk 'NR>1{print "  " $NF " (" $5 ")"}'
if [ -d "${RUN_DIR}/tombstones" ]; then
    echo "Tombstones  : $(ls "${RUN_DIR}/tombstones/" 2>/dev/null | wc -l)"
fi
echo ""
echo "Done."
