#!/usr/bin/env bash
set -e

# FortressOS dmesg severity level & facility filter test script
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "=== Running Host ASan/UBSan Unit Tests ==="
make test-dmesg-filter-host

echo ""
echo "=== Building Kernel & Boot Images ==="
make -j"$(nproc)"

echo ""
echo "=== Running QEMU Multi-Firmware Integration Suite ==="
python3 scripts/test_dmesg_filter.py

echo ""
echo "=== ALL DMESG SEVERITY LEVEL TESTS PASSED (100%) ==="
