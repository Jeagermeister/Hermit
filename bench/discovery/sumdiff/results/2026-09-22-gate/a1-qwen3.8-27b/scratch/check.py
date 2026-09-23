#!/usr/bin/env python3
"""The sum-difference objective, and nothing else.

    Gamma(A) = log(|A+A| / |A|) / log(|A-A| / |A|)      higher is better

Pure function of the printed set: no timing, no randomness, no model. The official scorer
(score.py) imports this file from the repository; attempt roots get a copy as `check.py` so
the model can see its own score, but that copy is never what the official score reads.

Run as a script it executes `python3 solve.py` in the current directory and prints the
score -- the development loop an attempt uses under the confined shell.

Stdlib only: the confined child sees /usr, not ~/.local (D10's grant table).
"""

import json
import math
import re
import subprocess
import sys

# Caps are part of the task, not tuning. They bound the scorer's cost: the sumset is built by
# OR-ing |A| shifted copies of a span-bit integer, so work is |A| * span / 64 words.
MAX_ELEMENTS = 20_000
MAX_SPAN = 1 << 20

_ALLOWED = re.compile(r"[\s,\[\](){}0-9-]*")
_INT = re.compile(r"-?\d+")


def parse(stdout: str):
    """Integers printed by solve.py -> (sorted distinct list, raw count) or a refusal string."""
    if not _ALLOWED.fullmatch(stdout):
        bad = next(c for c in stdout if not _ALLOWED.fullmatch(c))
        return None, f"stdout contains a character that is not part of an integer list: {bad!r}"
    raw = [int(t) for t in _INT.findall(stdout)]
    distinct = sorted(set(raw))
    if len(distinct) < 2:
        return None, f"need at least 2 distinct integers, got {len(distinct)}"
    if len(distinct) > MAX_ELEMENTS:
        return None, f"{len(distinct)} distinct integers exceeds the cap of {MAX_ELEMENTS}"
    span = distinct[-1] - distinct[0]
    if span > MAX_SPAN:
        return None, f"span max-min = {span} exceeds the cap of {MAX_SPAN}"
    return (distinct, len(raw)), None


def sizes(a):
    """(|A|, |A+A|, |A-A|) exactly, for sorted distinct `a`."""
    lo = a[0]
    shifted = [x - lo for x in a]
    span = shifted[-1]
    mask = 0
    rmask = 0
    for x in shifted:
        mask |= 1 << x
        rmask |= 1 << (span - x)
    sums = 0
    diffs = 0
    for x in shifted:
        sums |= mask << x            # {x + y}
        diffs |= rmask << x          # {x - y + span}
    return len(a), sums.bit_count(), diffs.bit_count()


def gamma(n: int, n_sum: int, n_diff: int) -> float:
    # |A-A| >= 2|A|-1 > |A| for |A| >= 2, so the denominator is strictly positive.
    return math.log(n_sum / n) / math.log(n_diff / n)


def evaluate(stdout: str) -> dict:
    parsed, reason = parse(stdout)
    if parsed is None:
        return {"valid": False, "reason": reason, "score": 0.0}
    distinct, n_raw = parsed
    n, n_sum, n_diff = sizes(distinct)
    return {
        "valid": True,
        "reason": None,
        "score": gamma(n, n_sum, n_diff),
        "n": n,
        "n_raw_tokens": n_raw,
        "span": distinct[-1] - distinct[0],
        "sumset_size": n_sum,
        "diffset_size": n_diff,
    }


if __name__ == "__main__":
    proc = subprocess.run([sys.executable, "solve.py"], capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        print(json.dumps({"valid": False, "reason": f"solve.py exited {proc.returncode}",
                          "stderr_tail": proc.stderr[-2000:]}, indent=2))
        sys.exit(1)
    print(json.dumps(evaluate(proc.stdout), indent=2))
