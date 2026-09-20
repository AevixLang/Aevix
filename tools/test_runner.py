#!/usr/bin/env python3
"""Quiet verbose runner: shows test name + status + percentage."""
import subprocess
import sys
import re

def main():
    args = sys.argv[1:]

    # collect count first
    collect = subprocess.run(
        [sys.executable, "-m", "pytest"] + args + ["--co", "-q"],
        capture_output=True, text=True
    )
    collected = len([l for l in collect.stdout.splitlines() if "::" in l])

    # run tests
    result = subprocess.run(
        [sys.executable, "-m", "pytest"] + args + ["-v", "--tb=short"],
        capture_output=True, text=True
    )

    idx = 0
    failed = []
    for line in result.stdout.splitlines():
        m = re.match(r"(.+?)::(\S+)\s+(PASSED|FAILED|ERROR)", line)
        if m:
            idx += 1
            name = m.group(2)
            ok = m.group(3) == "PASSED"
            pct = round(idx / collected * 100)
            tag = "ok" if ok else "FAIL"
            print(f"  [{tag}] {name}  ({pct}%)")
            if not ok:
                failed.append(name)

    print()
    if failed:
        print(f"Failed ({len(failed)}/{collected}): {', '.join(failed)}")
        print(result.stderr[-2000:] if result.stderr else "")
        sys.exit(1)
    else:
        print(f"All {collected} tests passed.")

if __name__ == "__main__":
    main()
