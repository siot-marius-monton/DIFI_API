#!/bin/bash
# Run difi_dpdk_receiver (primary) and sender_C_example (secondary) with ASLR disabled
# so shared mempool/rings work. Matches the pipeline in docs/DIFI_IQ_Dataflow.md.
# Use from DIFI_API directory:
#   sudo ./run_multi_process.sh
# Optional: DIFI_DEST=host:port (default 127.0.0.1:50000)
# Optional: DIFI_MAX_RATE=1 to run sender with --no-rate-limit (max throughput; receiver must keep up)
#
# To run module_b (validation-only consumer) instead of the DIFI sender:
#   sudo setarch $(uname -m) -R ./module_b/build/module_b ... & sudo setarch ... ./sender_C_example/build/sender_C_example ...
#
# If the secondary segfaults on NXP/ARM, use single-process demo instead:
#   cd single_process_demo/build && sudo ./single_process_demo -l 0 -- --streams 1 --chunk-ms 2

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH=$(uname -m)
# Some systems use different arch names for setarch
case "$ARCH" in
  aarch64|arm64) SETARCH_ARCH="aarch64" ;;
  x86_64|amd64)  SETARCH_ARCH="x86_64" ;;
  *)             SETARCH_ARCH="$ARCH" ;;
esac

# With --legacy-mem, EAL docs require -m to limit hugepage use (same for primary and secondary)
EAL_MEM="-m 512"
EAL_OPTS="--proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem $EAL_MEM"
EAL_OPTS_SEC="--proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem $EAL_MEM"
# For multi-worker sender use --workers N and add N lcores to EAL (e.g. -l 0,1,2,3 for 4 workers)
APP_OPTS="--streams 16 --chunk-ms 2"
DEST="${DIFI_DEST:-127.0.0.1:50000}"
SENDER_APP_OPTS="$APP_OPTS"
[ -n "${DIFI_MAX_RATE}" ] && [ "$DIFI_MAX_RATE" != "0" ] && SENDER_APP_OPTS="$SENDER_APP_OPTS --no-rate-limit"

RECEIVER="${SCRIPT_DIR}/difi_dpdk_receiver/build/difi_dpdk_receiver"
SENDER="${SCRIPT_DIR}/sender_C_example/build/sender_C_example"

for f in "$RECEIVER" "$SENDER"; do
  if [ ! -x "$f" ]; then
    echo "Error: $f not found or not executable. Build difi_dpdk_receiver and sender_C_example first." >&2
    exit 1
  fi
done

echo "Starting difi_dpdk_receiver (primary) dest=$DEST with setarch ${SETARCH_ARCH} -R ..."
sudo setarch "$SETARCH_ARCH" -R "$RECEIVER" $EAL_OPTS -- $APP_OPTS --dest "$DEST" &
B_PID=$!
sleep 3
if ! kill -0 $B_PID 2>/dev/null; then
  echo "Error: difi_dpdk_receiver exited. Check above for errors." >&2
  exit 1
fi

echo "Starting sender_C_example (secondary) with setarch ${SETARCH_ARCH} -R ..."
echo "Press Ctrl+C to stop both."
cleanup() { sudo kill $B_PID 2>/dev/null || true; }
trap cleanup EXIT INT TERM

set +e
sudo setarch "$SETARCH_ARCH" -R "$SENDER" $EAL_OPTS_SEC -- $SENDER_APP_OPTS
SENDER_EXIT=$?
set -e
if [ $SENDER_EXIT -ne 0 ]; then
  echo "" >&2
  echo "Secondary exited with code $SENDER_EXIT (segfault = 139 on many systems)." >&2
  echo "On NXP/ARM, DPDK multi-process shared mempool often fails; use single-process demo instead:" >&2
  echo "  cd $SCRIPT_DIR/single_process_demo/build && sudo ./single_process_demo -l 0 -- --streams 1 --chunk-ms 2" >&2
  exit $SENDER_EXIT
fi
