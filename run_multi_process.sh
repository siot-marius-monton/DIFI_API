#!/bin/bash
# Run module_b (primary) and sender_C_example (secondary) with ASLR disabled
# so shared mempool/rings work. Use from DIFI_API directory:
#   sudo ./run_multi_process.sh
#
# To run the DIFI-over-UDP receiver instead of module_b, use:
#   sudo ./difi_dpdk_receiver/run_difi_receiver.sh
#
# If the secondary still segfaults on NXP/ARM, multi-process shared mempool
# may not map correctly on that platform; use single-process demo instead:
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
# --use-shm: chunk data in POSIX shm (same VA in both); avoids DPDK mempool mapping issues on NXP/ARM
APP_OPTS="--streams 16 --chunk-ms 2 --use-shm"

MODULE_B="${SCRIPT_DIR}/module_b/build/module_b"
SENDER="${SCRIPT_DIR}/sender_C_example/build/sender_C_example"

for f in "$MODULE_B" "$SENDER"; do
  if [ ! -x "$f" ]; then
    echo "Error: $f not found or not executable. Build module_b and sender_C_example first." >&2
    exit 1
  fi
done

echo "Starting module_b (primary) with setarch ${SETARCH_ARCH} -R ..."
sudo setarch "$SETARCH_ARCH" -R "$MODULE_B" $EAL_OPTS -- $APP_OPTS &
B_PID=$!
sleep 3
if ! kill -0 $B_PID 2>/dev/null; then
  echo "Error: module_b exited. Check above for errors." >&2
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
  echo "Secondary exited with code $SENDER_EXIT (segfault = 139 on many systems)." >&2
  echo "On NXP/ARM, DPDK multi-process shared mempool often fails; use single-process demo instead:" >&2
  echo "  cd $SCRIPT_DIR/single_process_demo/build && sudo ./single_process_demo -l 0 -- --streams 1 --chunk-ms 2" >&2
  exit $SENDER_EXIT
fi
