#!/usr/bin/env python3
"""Point the [nigiri] block of motis' .pkg at a branch/commit.

Usage:  tools/rt-coverage-bench-set-nigiri.py <branch> <commit>

A plain `sed 's/branch=master/.../'` would rewrite every other dep on
branch=master as well, so this edits only the [nigiri] block.
"""
import os
import re
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)

branch, commit = sys.argv[1], sys.argv[2]
root = os.environ.get("MOTIS") or os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))
p = os.path.join(root, ".pkg")

new = ("[nigiri]\n"
       "  url=git@github.com:mority/nigiri.git\n"
       f"  branch={branch}\n"
       f"  commit={commit}\n")
s, n = re.subn(r"\[nigiri\]\n  url=[^\n]*\n  branch=[^\n]*\n  commit=[^\n]*\n",
               new, open(p).read())
assert n == 1, f"expected exactly one [nigiri] block in {p}, replaced {n}"
open(p, "w").write(s)
print(f"{p}:\n{new}")
