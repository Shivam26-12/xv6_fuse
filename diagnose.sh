#!/bin/bash
# diagnose.sh - Run this in WSL to show the exact function signatures in your xv6
# Usage: bash diagnose.sh [path-to-xv6-riscv]
ROOT="${1:-$HOME/xv6-riscv}"
echo "=== defs.h: sleep, wakeup, copyout, copyin, either_copy ==="
grep -n "sleep\|wakeup\|copyout\|copyin\|either_copy" "$ROOT/kernel/defs.h"
echo ""
echo "=== proc.c: actual sleep signature ==="
grep -n "^fuse_sleep\|^void sleep\|^void  sleep\|^ksleep\|^tsleep" "$ROOT/kernel/proc.c"
echo ""
echo "=== vm.c: actual copyout/copyin signatures ==="
grep -n "^int copyout\|^int copyin\|^int either" "$ROOT/kernel/vm.c"
echo ""
echo "=== Current defs.h around line 95-105 ==="
sed -n '90,110p' "$ROOT/kernel/defs.h"
echo ""
echo "=== Current defs.h around line 160-175 ==="
sed -n '158,178p' "$ROOT/kernel/defs.h"
