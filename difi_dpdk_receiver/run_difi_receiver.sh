#!/bin/bash
# Run difi_dpdk_receiver (primary) and sender_C_example (secondary) with ASLR disabled.
# Use from DIFI_API directory:
#   sudo ./difi_dpdk_receiver/run_difi_receiver.sh
# Optional: DIFI_DEST=host:port (default 127.0.0.1:50000)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
API_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARCH=$(uname -m)
case "$ARCH" in
  aarch64|arm64) SETARCH_ARCH="aarch64" ;;
  x86_64|amd64)  SETARCH_ARCH="x86_64" ;;
  *)             SETARCH_ARCH="$ARCH" ;;
esac

EAL_MEM="-m 512"
EAL_OPTS="--proc-type=primary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem $EAL_MEM"
EAL_OPTS_SEC="--proc-type=secondary --file-prefix=iqdemo --base-virtaddr=0x2000000000 --legacy-mem $EAL_MEM"
APP_OPTS="--streams 16 --chunk-ms 2 --use-shm"
DEST="${DIFI_DEST:-127.0.0.1:50000}"

RECEIVER="${SCRIPT_DIR}/build/difi_dpdk_receiver"
SENDER="${API_DIR}/sender_C_example/build/sender_C_example"

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
sudo setarch "$SETARCH_ARCH" -R "$SENDER" $EAL_OPTS_SEC -- $APP_OPTS
SENDER_EXIT=$?
set -e
if [ $SENDER_EXIT -ne 0 ]; then
  echo "" >&2
  echo "Secondary exited with code $SENDER_EXIT." >&2
  exit $SENDER_EXIT
fi
