#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/hil/run_hil_matrix.sh [options]

Options:
  --mode <mock|physical|all>        Campaign mode (default: mock)
  --iface <linux:ifname>            Physical transport spec (required for physical/all)
  --build-dir <dir>                 Build directory with binaries (default: build-local)
  --out-dir <dir>                   Output directory (default: artifacts/hil/<timestamp>)
  --mailbox-cycles <n>              mailbox_soak_demo cycles (default: 2000)
  --dc-duration-sec <n>             dc_soak_demo duration seconds (default: 300)
  --dc-period-us <n>                dc_soak_demo cycle period us (default: 1000)
  --with-sudo                       Run hardware demos via sudo
  --help                            Show this help

Examples:
  scripts/hil/run_hil_matrix.sh --mode mock
  scripts/hil/run_hil_matrix.sh --mode physical --iface linux:enp2s0 --with-sudo
  scripts/hil/run_hil_matrix.sh --mode all --iface linux:enp2s0 --out-dir artifacts/hil/nightly
EOF
}

MODE="mock"
IFACE=""
BUILD_DIR="build-local"
OUT_DIR=""
MAILBOX_CYCLES="2000"
DC_DURATION_SEC="300"
DC_PERIOD_US="1000"
WITH_SUDO="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --iface)
      IFACE="${2:-}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    --mailbox-cycles)
      MAILBOX_CYCLES="${2:-}"
      shift 2
      ;;
    --dc-duration-sec)
      DC_DURATION_SEC="${2:-}"
      shift 2
      ;;
    --dc-period-us)
      DC_PERIOD_US="${2:-}"
      shift 2
      ;;
    --with-sudo)
      WITH_SUDO="1"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ "$MODE" != "mock" && "$MODE" != "physical" && "$MODE" != "all" ]]; then
  echo "Invalid --mode: $MODE" >&2
  exit 2
fi

if [[ "$MODE" != "mock" && -z "$IFACE" ]]; then
  echo "--iface is required for mode=$MODE" >&2
  exit 2
fi

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="artifacts/hil/$(date +%Y%m%d_%H%M%S)"
fi

mkdir -p "$OUT_DIR"/logs

SUMMARY_CSV="$OUT_DIR/summary.csv"
SUMMARY_MD="$OUT_DIR/summary.md"
ENV_TXT="$OUT_DIR/environment.txt"

echo "case,exit_code,log" > "$SUMMARY_CSV"

if [[ "$WITH_SUDO" == "1" ]]; then
  RUNNER=(sudo)
else
  RUNNER=()
fi

write_environment() {
  {
    echo "timestamp=$(date -Iseconds)"
    echo "mode=$MODE"
    echo "iface=$IFACE"
    echo "build_dir=$BUILD_DIR"
    echo "mailbox_cycles=$MAILBOX_CYCLES"
    echo "dc_duration_sec=$DC_DURATION_SEC"
    echo "dc_period_us=$DC_PERIOD_US"
    echo "uname=$(uname -a)"
    if command -v lscpu >/dev/null 2>&1; then
      echo "--- lscpu ---"
      lscpu
    fi
    if command -v ethtool >/dev/null 2>&1 && [[ -n "$IFACE" ]]; then
      local ifname="${IFACE#linux:}"
      echo "--- ethtool $ifname ---"
      ethtool "$ifname" || true
    fi
  } > "$ENV_TXT"
}

run_case() {
  local name="$1"
  shift
  local logfile="$OUT_DIR/logs/${name}.log"

  echo "== Running: $name"
  set +e
  "$@" >"$logfile" 2>&1
  local code=$?
  set -e

  echo "$name,$code,$logfile" >> "$SUMMARY_CSV"
  if [[ $code -ne 0 ]]; then
    echo "   FAILED ($code): $name"
  else
    echo "   PASSED: $name"
  fi
}

write_environment

# Software-only regression gates (always run).
run_case "advanced_systems_tests" "$BUILD_DIR/advanced_systems_tests"
run_case "coe_mailbox_protocol_tests" "$BUILD_DIR/coe_mailbox_protocol_tests"
run_case "transport_module_boundary_tests" "$BUILD_DIR/transport_module_boundary_tests"

if [[ "$MODE" == "mock" || "$MODE" == "all" ]]; then
  run_case "mock_hil_soak" "$BUILD_DIR/mock_hil_soak"
  run_case "hil_conformance_demo" "$BUILD_DIR/hil_conformance_demo"
  run_case "redundancy_fault_sequence_demo_mock_json" \
    env OEC_SOAK_JSON=1 "$BUILD_DIR/redundancy_fault_sequence_demo"
fi

if [[ "$MODE" == "physical" || "$MODE" == "all" ]]; then
  run_case "physical_topology_scan_demo" "${RUNNER[@]}" "$BUILD_DIR/physical_topology_scan_demo" "$IFACE"
  run_case "mailbox_soak_demo_json" \
    "${RUNNER[@]}" env OEC_SOAK_JSON=1 "$BUILD_DIR/mailbox_soak_demo" "$IFACE" 1 0x1018 0x01 "$MAILBOX_CYCLES"
  run_case "dc_soak_demo_json" \
    "${RUNNER[@]}" env OEC_SOAK_JSON=1 "$BUILD_DIR/dc_soak_demo" "$IFACE" "$DC_DURATION_SEC" "$DC_PERIOD_US"
  run_case "topology_reconcile_demo_json" \
    "${RUNNER[@]}" env OEC_SOAK_JSON=1 OEC_TOPOLOGY_POLICY_ENABLE=1 "$BUILD_DIR/topology_reconcile_demo" "$IFACE"
  run_case "redundancy_fault_sequence_demo_json" \
    "${RUNNER[@]}" env OEC_SOAK_JSON=1 OEC_TOPOLOGY_POLICY_ENABLE=1 "$BUILD_DIR/redundancy_fault_sequence_demo" "$IFACE"
fi

{
  echo "# HIL Matrix Summary"
  echo
  echo "- Output directory: \`$OUT_DIR\`"
  echo "- Environment file: \`$ENV_TXT\`"
  echo
  echo "| Case | Exit Code | Log |"
  echo "|---|---:|---|"
  tail -n +2 "$SUMMARY_CSV" | while IFS=, read -r case_name exit_code log_path; do
    echo "| $case_name | $exit_code | \`$log_path\` |"
  done
  echo
  echo "Use this summary with \`docs/hil-validation-matrix.md\` acceptance gates."
} > "$SUMMARY_MD"

echo
echo "HIL matrix run finished."
echo "Summary: $SUMMARY_MD"

