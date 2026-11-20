#!/bin/bash
# quickfix.sh - Run this in WSL to diagnose & fix compilation errors
# Usage: bash quickfix.sh [path-to-xv6-riscv]
set -e
ROOT="${1:-$HOME/xv6-riscv}"

echo "=== STEP 1: Diagnosing your xv6 signatures ==="
echo ""

echo "--- sleep signature (proc.c) ---"
grep -n "^void sleep\|^void  sleep\|^static.*sleep" "$ROOT/kernel/proc.c" | head -5

echo ""
echo "--- copyout/copyin signatures (vm.c) ---"
grep -n "^int copyout\|^int copyin\|^int either" "$ROOT/kernel/vm.c" | head -6

echo ""
echo "--- defs.h lines 90-115 (sleep/wakeup area) ---"
sed -n '90,115p' "$ROOT/kernel/defs.h"

echo ""
echo "--- defs.h lines 160-180 (vm area) ---"
sed -n '160,180p' "$ROOT/kernel/defs.h"

echo ""
echo "=== STEP 2: Auto-fixing defs.h if sleep is wrong ==="

# Check if defs.h has 'void sleep(void)' (wrong) and fix it
if grep -q "void[[:space:]]*sleep[[:space:]]*([[:space:]]*void[[:space:]]*)" "$ROOT/kernel/defs.h"; then
    echo "FIXING: found bad 'sleep(void)' in defs.h, replacing with correct signature..."
    sed -i 's/void[[:space:]]*sleep[[:space:]]*([[:space:]]*void[[:space:]]*);/void            sleep(void*, struct spinlock*);/' "$ROOT/kernel/defs.h"
    echo "FIXED: sleep(void*, struct spinlock*)"
else
    echo "OK: sleep signature looks fine"
fi

echo ""
echo "=== STEP 3: Checking copyout in file.c for arg count mismatch ==="
COPYOUT_ARGS=$(grep -c "," <(grep "^int copyout" "$ROOT/kernel/vm.c") 2>/dev/null || echo "3")
echo "copyout has approximately $((COPYOUT_ARGS + 1)) arguments"

echo ""
echo "=== Done. Now try: cd $ROOT && make clean && make qemu ==="
