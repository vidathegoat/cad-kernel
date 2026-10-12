"""Billet benchmark runner.

Runs the C++ harness (bench/harness) once per job, collects timings, peak memory and output
fingerprints, and writes one JSON results file. Standard library only.

Commands:
  fetch     download the corpus listed in bench/corpus.manifest.json into bench/corpus/
  run       run the benchmark and write bench/results/<date>-<commit>.json (or --label <name>)
  compare   compare two results files: timing changes and output fingerprints

Examples:
  python bench/runner/billet_bench.py fetch
  python bench/runner/billet_bench.py run
  python bench/runner/billet_bench.py run --label baseline-8.0.1
  python bench/runner/billet_bench.py compare bench/results/baseline-8.0.1.json bench/results/<new>.json
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

RUNNER_VERSION = "0.1.0"
REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "bench"
MANIFEST = BENCH / "corpus.manifest.json"
CORPUS = BENCH / "corpus"
RESULTS = BENCH / "results"
DEFAULT_HARNESS = REPO / "build-bench" / "Release" / "billet-bench.exe"
DEFAULT_INSTALL = Path(os.environ.get("BILLET_INSTALL", "C:/billet-install"))
DEFAULT_DEPS_BIN = REPO / "build" / "vcpkg_installed" / "x64-windows" / "bin"

# Job types: harness operation plus fixed parameters. Model-specific parameters come from the manifest.
JOB_TYPES = {
    "step-read": ("step-read", {}),
    "step-write": ("step-write", {}),
    "bool-fuse": ("bool-fuse", {}),
    "bool-cut": ("bool-cut", {}),
    "bool-common": ("bool-common", {}),
    "fillet": ("fillet", {}),
    "shell": ("shell", {}),
    "mesh-coarse": ("mesh", {"deflection": "coarse"}),
    "mesh-fine": ("mesh", {"deflection": "fine"}),
    "check": ("check", {}),
}
# Jobs that need parameters from the manifest; they run only for models that provide them.
NEEDS_PARAMS = {"fillet", "shell"}
REBUILD = "rebuild"
ALL_JOBS = [REBUILD, *JOB_TYPES]
THREAD_MODES = ["1", "all"]


def log(message: str) -> None:
    print(message, flush=True)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path = MANIFEST) -> dict | None:
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


# --------------------------------------------------------------------------- fetch

def download(url: str, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": f"billet-bench/{RUNNER_VERSION}"})
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False, suffix=".part") as temp:
        temp_path = Path(temp.name)
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                shutil.copyfileobj(response, temp, length=1 << 20)
        except BaseException:
            temp.close()
            temp_path.unlink(missing_ok=True)
            raise
    temp_path.replace(target)


def fetch_model(model: dict, update: bool) -> tuple[bool, str]:
    target = CORPUS / model["file"]
    archive = model.get("archive")
    if not target.exists():
        if archive:
            zip_path = CORPUS / "_downloads" / Path(archive.get("file", model["id"] + ".zip")).name
            if not zip_path.exists():
                log(f"  downloading {model['url']}")
                download(model["url"], zip_path)
            with zipfile.ZipFile(zip_path) as bundle, bundle.open(archive["member"]) as source:
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("wb") as sink:
                    shutil.copyfileobj(source, sink, length=1 << 20)
        else:
            log(f"  downloading {model['url']}")
            download(model["url"], target)
    actual = sha256_of(target)
    size = target.stat().st_size
    expected = model.get("sha256")
    if not expected:
        if update:
            model["sha256"] = actual
            model["bytes"] = size
            return True, f"recorded sha256 {actual[:12]}..., {size} bytes"
        return False, f"no sha256 in manifest (actual {actual}); rerun with --update to record it"
    if actual != expected:
        return False, f"checksum mismatch: expected {expected}, got {actual}"
    return True, f"ok ({size} bytes)"


def command_fetch(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path)
    if manifest is None:
        log(f"No manifest at {manifest_path}")
        return 1
    wanted = set(args.models.split(",")) if args.models else None
    failures = 0
    for model in manifest["models"]:
        if wanted and model["id"] not in wanted:
            continue
        log(f"{model['id']}:")
        try:
            ok, message = fetch_model(model, args.update)
        except Exception as error:  # network or archive errors are reported per model
            ok, message = False, f"{type(error).__name__}: {error}"
        log(f"  {message}")
        failures += 0 if ok else 1
    if args.update:
        MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
        log(f"Updated {MANIFEST.relative_to(REPO)}")
    log(f"{failures} model(s) failed" if failures else "All models present and verified")
    return 1 if failures else 0


# --------------------------------------------------------------------------- run

def git_info() -> dict:
    def git(*command: str) -> str:
        return subprocess.run(["git", *command], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()

    try:
        status = git("status", "--porcelain", "--untracked-files=no")
        return {"commit": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
                "dirty": bool(status)}
    except (OSError, subprocess.CalledProcessError):
        return {"commit": "unknown", "branch": "unknown", "dirty": True}


def machine_info() -> dict:
    info = {"name": platform.node(), "os": platform.platform(), "logical_cpus": os.cpu_count()}
    if sys.platform == "win32":
        import ctypes
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                info["cpu"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        except OSError:
            pass

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = MemoryStatus()
        status.dwLength = ctypes.sizeof(MemoryStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            info["ram_bytes"] = status.ullTotalPhys
        try:
            plan = subprocess.run(["powercfg", "/getactivescheme"], capture_output=True, text=True, check=True)
            info["power_plan"] = plan.stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    return info


def harness_env(args: argparse.Namespace) -> dict:
    env = dict(os.environ)
    paths = [str(Path(args.install) / "win64" / "vc14" / "bin"), str(args.deps_bin)]
    env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    return env


def scripted_parts(args: argparse.Namespace, env: dict) -> list[str]:
    output = subprocess.run([str(args.harness), "parts"], capture_output=True, text=True, env=env, check=True)
    return [line.split("\t")[0] for line in output.stdout.splitlines() if line.strip()]


def plan_jobs(args: argparse.Namespace, manifest: dict | None, parts: list[str]) -> list[dict]:
    wanted_jobs = args.jobs.split(",") if args.jobs else ALL_JOBS
    wanted_models = set(args.models.split(",")) if args.models else None
    threads = args.threads.split(",")
    unknown = [job for job in wanted_jobs if job not in ALL_JOBS]
    if unknown:
        raise SystemExit(f"Unknown job type(s): {', '.join(unknown)}. Known: {', '.join(ALL_JOBS)}")

    jobs = []
    if REBUILD in wanted_jobs:
        for part in parts:
            if wanted_models and f"part:{part}" not in wanted_models:
                continue
            for mode in threads:
                jobs.append({"job": REBUILD, "op": REBUILD, "model": f"part:{part}", "harness_model": part,
                             "threads": mode, "params": {}})
    if manifest is None:
        if any(job != REBUILD for job in wanted_jobs):
            log(f"No corpus manifest at {args.manifest}: running rebuild jobs only")
        return jobs

    for model in manifest["models"]:
        if wanted_models and model["id"] not in wanted_models:
            continue
        model_ops = model.get("ops", {})
        skipped = set(model.get("skip", []))
        for job in wanted_jobs:
            if job == REBUILD or job in skipped:
                continue
            if job in NEEDS_PARAMS and job not in model_ops:
                continue
            op, fixed = JOB_TYPES[job]
            params = {**fixed, **{k: str(v) for k, v in model_ops.get(job, {}).items()}}
            for mode in threads:
                jobs.append({"job": job, "op": op, "model": model["id"], "harness_model": str(CORPUS / model["file"]),
                             "threads": mode, "params": params})
    return jobs


def run_job(job: dict, args: argparse.Namespace, env: dict, output_dir: Path) -> dict:
    stem = f"{job['model'].replace(':', '-')}.{job['job']}.{'mt' if job['threads'] == 'all' else 'st'}"
    report = output_dir / f"{stem}.json"
    saved = output_dir / (f"{stem}.step" if job["op"] == "step-write" else f"{stem}.brep")
    command = [str(args.harness), "run", "--op", job["op"], "--model", job["harness_model"],
               "--threads", job["threads"], "--warmup", str(args.warmup), "--runs", str(args.runs),
               "--cpu", str(args.cpu), "--save-output", str(saved), "--scratch", str(output_dir), "--out", str(report)]
    for key, value in job["params"].items():
        command += ["--param", f"{key}={value}"]
    if args.no_validity:
        command.append("--no-validity")

    flags = 0
    if sys.platform == "win32" and args.priority == "high":
        flags = subprocess.HIGH_PRIORITY_CLASS
    report.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.timeout,
                                 creationflags=flags)
        exit_code = process.returncode
        stderr = process.stderr.strip()
    except subprocess.TimeoutExpired:
        exit_code, stderr = None, ""
    elapsed = time.monotonic() - started

    result = {key: job[key] for key in ("job", "op", "model", "threads", "params")}
    if exit_code is None:
        result.update(status="timeout", error=f"exceeded {args.timeout} s")
    elif not report.exists():
        result.update(status="crash", error=f"exit code {exit_code}; {stderr[-500:]}")
    else:
        data = json.loads(report.read_text(encoding="utf-8"))
        for key in ("status", "phase", "error", "times_s", "median_s", "memory", "details", "fingerprint"):
            if key in data:
                result[key] = data[key]
        if data.get("status") == "ok" and saved.exists():
            result["output"] = {"file": str(saved.relative_to(REPO)).replace("\\", "/"),
                                "bytes": saved.stat().st_size, "sha256": sha256_of(saved)}
    result["job_wall_s"] = round(elapsed, 3)
    return result


def summarize(jobs: list[dict]) -> dict:
    summary = {"jobs": len(jobs), "ok": sum(1 for job in jobs if job.get("status") == "ok")}
    summary["failed"] = summary["jobs"] - summary["ok"]
    by_type: dict[str, dict] = {}
    for job in jobs:
        if job.get("status") != "ok":
            continue
        key = f"{job['job']}/{job['threads']}"
        entry = by_type.setdefault(key, {"jobs": 0, "sum_median_s": 0.0, "_logs": []})
        entry["jobs"] += 1
        entry["sum_median_s"] += job["median_s"]
        entry["_logs"].append(math.log(max(job["median_s"], 1e-9)))
    for entry in by_type.values():
        logs = entry.pop("_logs")
        entry["geomean_median_s"] = math.exp(sum(logs) / len(logs))
    summary["by_job"] = dict(sorted(by_type.items()))
    return summary


def command_run(args: argparse.Namespace) -> int:
    args.harness = Path(args.harness)
    if not args.harness.exists():
        log(f"Harness not found: {args.harness}. Build it first (see bench/harness/CMakeLists.txt).")
        return 1
    env = harness_env(args)
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path)
    if manifest is not None:
        missing = [model["id"] for model in manifest["models"] if not (CORPUS / model["file"]).exists()]
        if missing and not args.models:
            log(f"Missing corpus files ({len(missing)}): run 'fetch' first. Missing: {', '.join(missing[:10])}")
            return 1

    git = git_info()
    now = datetime.datetime.now().astimezone()
    label = args.label or f"{now:%Y-%m-%d}-{git['commit'][:10]}{'-dirty' if git['dirty'] else ''}"
    results_path = Path(args.output) if args.output else RESULTS / f"{label}.json"
    output_dir = CORPUS / "_outputs" / label
    output_dir.mkdir(parents=True, exist_ok=True)

    jobs = plan_jobs(args, manifest, scripted_parts(args, env))
    version = subprocess.run([str(args.harness), "version"], capture_output=True, text=True, env=env).stdout.strip()
    log(f"Running {len(jobs)} job(s); results -> {results_path.relative_to(REPO) if results_path.is_relative_to(REPO) else results_path}")

    results = []
    for index, job in enumerate(jobs, 1):
        result = run_job(job, args, env, output_dir)
        results.append(result)
        status = result.get("status")
        detail = f"{result['median_s']:.4f} s" if status == "ok" else f"{status}: {result.get('error', '')}"
        log(f"[{index}/{len(jobs)}] {job['job']:<12} {job['model']:<28} threads={job['threads']:<3} {detail}")

    document = {
        "schema_version": 1,
        "kind": "benchmark",
        "label": label,
        "created": now.isoformat(timespec="seconds"),
        "git": git,
        "machine": machine_info(),
        "harness": {"version_line": version, "runner_version": RUNNER_VERSION},
        "settings": {"warmup": args.warmup, "runs": args.runs, "threads": args.threads.split(","),
                     "single_thread_cpu": args.cpu, "priority": args.priority,
                     "validity_checked": not args.no_validity},
        "corpus": None if manifest is None else {"manifest": str(manifest_path),
                                                 "manifest_sha256": sha256_of(manifest_path),
                                                 "models": len(manifest["models"])},
        "summary": summarize(results),
        "jobs": results,
    }
    results_path.parent.mkdir(parents=True, exist_ok=True)
    results_path.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8", newline="\n")
    summary = document["summary"]
    log(f"Done: {summary['ok']} ok, {summary['failed']} failed. Wrote {results_path}")
    return 0 if summary["failed"] == 0 else 2


# --------------------------------------------------------------------------- compare

FINGERPRINT_KEYS = ("volume", "area", "bbox", "solids", "shells", "faces", "edges", "vertices",
                    "triangles", "nodes", "valid")


def close(a, b, tolerance: float) -> bool:
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tolerance) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        if a is None or b is None:
            return a is b
        return math.isclose(a, b, rel_tol=tolerance, abs_tol=tolerance)
    return a == b


def command_compare(args: argparse.Namespace) -> int:
    base = json.loads(Path(args.base).read_text(encoding="utf-8"))
    new = json.loads(Path(args.new).read_text(encoding="utf-8"))
    key = lambda job: (job["job"], job["model"], job["threads"])  # noqa: E731
    base_jobs = {key(job): job for job in base["jobs"]}
    new_jobs = {key(job): job for job in new["jobs"]}

    rows, mismatches, noisy, missing = [], [], [], []
    ratios: dict[str, list[float]] = {}
    for job_key in sorted(base_jobs.keys() | new_jobs.keys()):
        old, cur = base_jobs.get(job_key), new_jobs.get(job_key)
        if old is None or cur is None or old.get("status") != "ok" or cur.get("status") != "ok":
            missing.append((job_key, old and old.get("status"), cur and cur.get("status")))
            continue
        ratio = cur["median_s"] / old["median_s"] if old["median_s"] > 0 else float("inf")
        ratios.setdefault(f"{job_key[0]}/{job_key[2]}", []).append(ratio)
        change = (ratio - 1.0) * 100.0
        rows.append((job_key, old["median_s"], cur["median_s"], change))
        if args.max_change is not None and abs(change) > args.max_change:
            noisy.append((job_key, change))

        differences = [field for field in FINGERPRINT_KEYS
                       if not close(old.get("fingerprint", {}).get(field), cur.get("fingerprint", {}).get(field),
                                    args.tolerance)]
        if args.exact_outputs:
            old_hash = old.get("output", {}).get("sha256")
            cur_hash = cur.get("output", {}).get("sha256")
            if old_hash != cur_hash:
                differences.append("output_sha256")
        if differences:
            mismatches.append((job_key, differences))

    log(f"{'job':<12} {'model':<28} {'thr':<4} {'base s':>10} {'new s':>10} {'change':>8}")
    for (job, model, threads), old_s, new_s, change in rows:
        log(f"{job:<12} {model:<28} {threads:<4} {old_s:>10.4f} {new_s:>10.4f} {change:>+7.1f}%")
    log("")
    log("Geometric mean of new/base per job type:")
    for job_type, values in sorted(ratios.items()):
        geomean = math.exp(statistics.fmean(math.log(value) for value in values))
        log(f"  {job_type:<20} {(geomean - 1.0) * 100.0:+6.1f}%  ({len(values)} jobs)")
    for job_key, old_status, new_status in missing:
        log(f"NOT COMPARED {job_key}: base={old_status} new={new_status}")
    for job_key, differences in mismatches:
        log(f"FINGERPRINT MISMATCH {job_key}: {', '.join(differences)}")
    for job_key, change in noisy:
        log(f"OVER {args.max_change}% {job_key}: {change:+.1f}%")
    failed = bool(mismatches) or bool(noisy)
    log("RESULT: " + ("FAIL" if failed else "PASS")
        + f" ({len(rows)} compared, {len(mismatches)} fingerprint mismatches, {len(noisy)} over the limit)")
    return 1 if failed else 0


# --------------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    fetch = commands.add_parser("fetch", help="download and verify the corpus")
    fetch.add_argument("--manifest", default=str(MANIFEST), help="corpus manifest (default: bench/corpus.manifest.json)")
    fetch.add_argument("--models", help="comma-separated model ids (default: all)")
    fetch.add_argument("--update", action="store_true", help="record sha256 and size for entries that lack them")
    fetch.set_defaults(handler=command_fetch)

    run = commands.add_parser("run", help="run the benchmark")
    run.add_argument("--manifest", default=str(MANIFEST), help="corpus manifest (default: bench/corpus.manifest.json)")
    run.add_argument("--label", help="results name (default: <date>-<commit>)")
    run.add_argument("--output", help="results file path (default: bench/results/<label>.json)")
    run.add_argument("--jobs", help=f"comma-separated job types (default: all): {', '.join(ALL_JOBS)}")
    run.add_argument("--models", help="comma-separated model ids, or part:<id> for rebuild parts")
    run.add_argument("--threads", default="1,all", help="thread modes (default: 1,all)")
    run.add_argument("--warmup", type=int, default=1)
    run.add_argument("--runs", type=int, default=5)
    run.add_argument("--cpu", type=int, default=2, help="logical CPU for single-threaded jobs (default: 2)")
    run.add_argument("--priority", choices=["normal", "high"], default="high")
    run.add_argument("--timeout", type=int, default=3600, help="seconds per job (default: 3600)")
    run.add_argument("--no-validity", action="store_true", help="skip BRepCheck on outputs")
    run.add_argument("--harness", default=str(DEFAULT_HARNESS))
    run.add_argument("--install", default=str(DEFAULT_INSTALL))
    run.add_argument("--deps-bin", default=str(DEFAULT_DEPS_BIN))
    run.set_defaults(handler=command_run)

    compare = commands.add_parser("compare", help="compare two results files")
    compare.add_argument("base")
    compare.add_argument("new")
    compare.add_argument("--max-change", type=float, help="fail if any timing changes by more than this percent")
    compare.add_argument("--tolerance", type=float, default=0.0,
                         help="relative tolerance for fingerprint numbers (0 = exact; 1e-9 for parallel changes)")
    compare.add_argument("--exact-outputs", action="store_true", help="also require identical output file hashes")
    compare.set_defaults(handler=command_compare)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
