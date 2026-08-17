#!/usr/bin/env python3
"""Run the six local-agent diagnostic stages under HERMES AGENT.

Companion to ~/Source/local-agent-benchmarks/integration-diagnostic, which ran the identical
stages under OpenCode on kitchen-desktop. The point is the DELTA between the two
harnesses: where the same model behaves differently, the cause is tool design, not
the model - and that is a requirement for Hermit.

Usage:
    ./run_hermit_diagnostic.py --models qwen-9b gemma-12b --repeats 3
    ./run_hermit_diagnostic.py --models gemma-12b --stages 01_read --repeats 1   # smoke test
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stages import STAGES  # noqa: E402

ROOT = Path(__file__).resolve().parent
RUNS, LOGS, RESULTS = ROOT / "runs", ROOT / "logs", ROOT / "results"

# Ollama tags. These must match the num_ctx-pinned variants built from
# tournament/models/*.Modelfile, or the comparison is not like-for-like.
MODELS = {
    "gemma-e4b": "tournament-gemma-e4b:32k",
    "qwen-9b": "tournament-qwen-9b:32k",
    "gemma-12b": "tournament-gemma-12b:32k",
}

HERMES = Path.home() / ".local" / "bin" / "hermes"
OLLAMA_BASE = "http://localhost:11434/v1"
TIMEOUT = 300


def write_stage(path: Path, stage: dict) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for name, content in stage["files"].items():
        (path / name).write_text(content, encoding="utf-8")


def pytest_result(path: Path) -> dict | None:
    """Score the working tree the same way the OpenCode harness did."""
    try:
        r = subprocess.run([sys.executable, "-m", "pytest", "-q"], cwd=path,
                           text=True, capture_output=True, timeout=30, check=False)
    except Exception as exc:
        return {"error": str(exc)}
    return {"returncode": r.returncode, "passed": r.returncode == 0,
            "tail": r.stdout.strip().splitlines()[-3:]}


def make_deterministic_variant(tag: str, seed: int) -> str:
    """Build a temperature-0, fixed-seed variant of `tag` and return its name.

    Sampling parameters are per-request or per-Modelfile in Ollama - there are no
    OLLAMA_TEMPERATURE / OLLAMA_SEED environment variables, and setting them would
    be silently ignored. Since Hermes owns the API call, a derived model is the
    only reliable way to pin sampling from out here.
    """
    det = f"{tag.split(':')[0]}-det:{seed}"
    # `ollama create -f -` does not read stdin, so the Modelfile has to hit disk.
    with tempfile.NamedTemporaryFile("w", suffix=".Modelfile", delete=False) as fh:
        fh.write(f"FROM {tag}\nPARAMETER temperature 0\nPARAMETER seed {seed}\n")
        mf = fh.name
    try:
        proc = subprocess.run(["ollama", "create", det, "-f", mf],
                              text=True, capture_output=True, timeout=180, check=False)
    finally:
        os.unlink(mf)
    if proc.returncode != 0:
        raise RuntimeError(f"could not build deterministic variant {det}: {proc.stderr.strip()[:200]}")
    return det


def run_stage(model_key: str, stage_name: str, rep: int, deterministic: bool = False,
              seed: int = 1337) -> dict:
    tag = MODELS[model_key]
    if deterministic:
        tag = make_deterministic_variant(tag, seed)
    stage = STAGES[stage_name]
    work = RUNS / f"{model_key}-{stage_name}-r{rep}"
    shutil.rmtree(work, ignore_errors=True)
    write_stage(work, stage)

    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f"{model_key}-{stage_name}-r{rep}.log"

    cmd = [str(HERMES), "-z", stage["prompt"], "--in", str(work),
           "-m", tag, "--provider", "custom", "--yolo"]
    # Determinism is opt-in on purpose. The OpenCode baseline ran at model-default
    # sampling with no seed, which is why identical configs there scored 6/6 and 4/6.
    # Matching that default keeps Phase 0 like-for-like; pinning it is for isolating
    # a genuine hardware or backend effect, where sampling noise would otherwise
    # swamp the signal.
    env_extra = {}
    env = {**os.environ, "PATH": f"{Path.home()}/.local/bin:{os.environ.get('PATH','')}", **env_extra}

    start = time.monotonic()
    timed_out = False
    with log.open("w") as out:
        try:
            proc = subprocess.run(cmd, cwd=work, env=env, stdout=out,
                                  stderr=subprocess.STDOUT, text=True,
                                  timeout=TIMEOUT, check=False)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc, timed_out = None, True
    elapsed = round(time.monotonic() - start, 2)

    # Hermes exits 0 even when the API call fails outright, so returncode is not a
    # sufficient success signal. Detect hard failures explicitly - otherwise a run that
    # never reached the model gets scored as a legitimate result, which is worse than
    # a crash because it looks like data.
    log_text = log.read_text(errors="replace")
    FAILURE_SIGNATURES = ("API call failed", "HTTP 404", "HTTP 500", "HTTP 502",
                          "Connection refused", "model .* not found", "Traceback")
    harness_error = next((sig for sig in FAILURE_SIGNATURES if sig in log_text), None)

    rec = {
        "model": model_key, "ollama_tag": tag, "stage": stage_name, "repeat": rep,
        "returncode": rc, "timed_out": timed_out, "seconds": elapsed,
        "harness_error": harness_error,
        "valid": harness_error is None and not timed_out,
        "log": str(log.relative_to(ROOT)),
        "worktree": str(work.relative_to(ROOT)),
        "when": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "deterministic": deterministic,
        "seed": seed if deterministic else None,
    }
    # 01_read and 02_edit have no tests; the OpenCode harness scored them the same way.
    if harness_error:
        # Do not score a run that never reached the model.
        return rec
    if stage_name not in {"01_read", "02_edit"}:
        rec["pytest"] = pytest_result(work)
    elif stage_name == "02_edit":
        settings = work / "settings.py"
        rec["mode_fixed"] = settings.exists() and "fixed" in settings.read_text()
    else:
        rec["marker_echoed"] = "MARKER-ORBIT-7319" in log_text
    return rec


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", nargs="+", default=list(MODELS), choices=list(MODELS))
    ap.add_argument("--stages", nargs="+", default=list(STAGES), choices=list(STAGES))
    ap.add_argument("--repeats", type=int, default=3,
                    help="3 minimum for real results - identical configs have scored 6/6 and 4/6")
    ap.add_argument("--out", default=None, help="results filename (default: timestamped)")
    ap.add_argument("--deterministic", action="store_true",
                    help="build temperature-0, fixed-seed model variants and run against those. "
                         "Removes sampling "
                         "noise, but makes results NOT comparable to the OpenCode baseline, "
                         "which ran at model-default sampling. Use for GPU A/B, not for Phase 0.")
    ap.add_argument("--seed", type=int, default=1337, help="seed used with --deterministic")
    ap.add_argument("--tag", default=None,
                    help="override the Ollama tag for ALL models (smoke testing only - "
                         "breaks comparability with the OpenCode baseline)")
    args = ap.parse_args()

    if not HERMES.exists():
        print(f"hermes not found at {HERMES} - install it on this machine first", file=sys.stderr)
        return 1
    if args.tag:
        for k in MODELS:
            MODELS[k] = args.tag
        print(f"WARNING: all models overridden to {args.tag}. Smoke test only - "
              "results are NOT comparable to the OpenCode baseline.\n", file=sys.stderr)
    else:
        try:
            have = subprocess.run(["ollama", "list"], capture_output=True, text=True,
                                  timeout=20, check=False).stdout
        except Exception:
            have = ""
        missing = [MODELS[m] for m in args.models if MODELS[m].split(":")[0] not in have]
        if missing:
            print("Missing Ollama models: " + ", ".join(missing), file=sys.stderr)
            print("Build them from tournament/models/*.Modelfile, e.g.:\n"
                  "  ollama create tournament-qwen-9b:32k -f "
                  "~/Source/local-agent-benchmarks/tournament/models/qwen-9b-32k.Modelfile",
                  file=sys.stderr)
            return 1

    if args.repeats < 3:
        print(f"WARNING: --repeats {args.repeats}. Run-to-run variance on identical inputs "
              "has produced 6/6 and 4/6. Treat anything under 3 as a smoke test.\n", file=sys.stderr)

    RESULTS.mkdir(parents=True, exist_ok=True)
    records = []
    total = len(args.models) * len(args.stages) * args.repeats
    n = 0
    for model in args.models:
        for stage in args.stages:
            for rep in range(1, args.repeats + 1):
                n += 1
                print(f"[{n}/{total}] {model} {stage} r{rep} ... ", end="", flush=True)
                rec = run_stage(model, stage, rep, args.deterministic, args.seed)
                records.append(rec)
                print(("INVALID: " + rec["harness_error"] + " " if rec.get("harness_error") else "")
                      + f"{rec['seconds']}s"
                      + (" TIMEOUT" if rec["timed_out"] else "")
                      + (f" pytest={'pass' if rec.get('pytest',{}).get('passed') else 'fail'}"
                         if "pytest" in rec else ""))

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = RESULTS / (args.out or f"hermes-diagnostic-{stamp}.json")
    out.write_text(json.dumps({
        "harness": "hermes",
        "note": "Compare against local-agent-benchmarks/integration-diagnostic (OpenCode). "
                "Same stages, same models. The delta is the finding.",
        "machine": os.uname().nodename,
        "repeats": args.repeats,
        "deterministic": args.deterministic,
        "gpu": os.environ.get("BENCH_GPU", "UNRECORDED - set BENCH_GPU to name the card"),
        "records": records,
    }, indent=2))
    invalid = [r for r in records if not r.get("valid")]
    if invalid:
        print(f"\n{len(invalid)}/{len(records)} runs were INVALID (never reached the model "
              "or timed out). Fix the cause and rerun - do not analyse partial data.")
    print(f"\nwrote {out.relative_to(ROOT)}")
    print("Machine:", os.uname().nodename,
          "- the OpenCode baseline was kitchen-desktop; different hardware confounds the comparison.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
