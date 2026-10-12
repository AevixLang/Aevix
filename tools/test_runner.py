#!/usr/bin/env python3
"""Quiet verbose runner: streams test results in real time."""
import subprocess
import sys
import re

def main():
    args = sys.argv[1:]

    proc = subprocess.Popen(
        [sys.executable, "-m", "pytest"] + args + ["-v", "--tb=short"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1
    )

    tests = []
    total = 0

    for line in proc.stdout:
        line = line.rstrip()

        # parse total from "N selected" or "collected N items"
        m_sel = re.search(r"(\d+) selected", line)
        if m_sel:
            total = int(m_sel.group(1))
            continue
        if total == 0:
            m_col = re.search(r"collected (\d+) items", line)
            if m_col:
                total = int(m_col.group(1))
                continue

        m = re.match(r"(.+?)::(\S+)\s+(PASSED|FAILED|ERROR)", line)
        if m:
            name = m.group(2)
            ok = m.group(3) == "PASSED"
            tests.append((name, ok))
            idx = len(tests)
            pct = round(idx / total * 100) if total else 0
            tag = "\033[32mok\033[0m" if ok else "\033[31mFAIL\033[0m"
            print(f"  {tag}  {name}  ({pct}%)")
            sys.stdout.flush()

    proc.wait()

    failed = [name for name, ok in tests if not ok]
    print()
    if failed:
        print(f"\033[31mFailed\033[0m ({len(failed)}/{total}): {', '.join(failed)}")
        sys.exit(1)
    else:
        print(f"\033[32mAll {total} tests passed.\033[0m")

if __name__ == "__main__":
    main()
