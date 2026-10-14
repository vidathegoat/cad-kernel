"""Billet profiler.

Profiles one benchmark job at a time with the command-line collector of Visual Studio's Performance
Profiler (VSDiagnostics.exe, CPU sampling with call stacks), resolves the harness's samples to
function names with DbgHelp, and reports hotspots (SPEC.md section 6). Standard library only; DbgHelp
is called through ctypes. Windows only.

Commands:
  record   profile one job, save its samples as <out-dir>/<model>.<job>.<st|mt>.json.gz
  report   print the hotspots of one or more saved profiles as Markdown

Examples:
  python bench/runner/billet_profile.py record --job bool-fuse --model freecad-cup
  python bench/runner/billet_profile.py report C:/billet-results/profiles/<label>/*.bool-fuse.st.json.gz

Only samples taken while the harness runs the operation count: on the main thread, samples whose
stack is inside the operation's run(); on other threads, samples taken between the first and last
such main-thread sample of each run. Setup, prepareRun() and the fingerprint are excluded.

Requirements: Visual Studio 2022 (the collector), the Windows Performance Toolkit (xperf.exe, which
turns the trace into text) and the Debugging Tools for Windows (dbghelp.dll and symsrv.dll). Billet
and the harness need PDB files; see docs/billet/decisions.md. System DLL symbols are downloaded
from Microsoft's symbol server into C:/billet-symbols.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import ctypes
import datetime
import glob
import gzip
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True  # no __pycache__ folder in the repository
sys.path.insert(0, str(Path(__file__).resolve().parent))
import billet_bench as bench  # noqa: E402  (job planning and paths are shared with the runner)

PROFILER_VERSION = "0.1.0"
COLLECTOR = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Team Tools\DiagnosticsHub"
                 r"\Collector\VSDiagnostics.exe")
XPERF = Path(r"C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe")
DBGHELP = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll")
SYMBOL_CACHE = Path("C:/billet-symbols/sym")
PROFILES = Path("C:/billet-results/profiles")
BASELINE = bench.RESULTS / "baseline-8.0.1.json"
CPU_AGENT = {"CLSID": "4EA90761-2248-496C-B854-3C0399A591A4", "Name": "DiagnosticsHub.CpuAgent.dll"}
KERNEL_SPACE = 0xFFFF800000000000


def log(message: str) -> None:
    print(message, flush=True)


# --------------------------------------------------------------------------- DbgHelp

class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG),
                ("Reserved", ctypes.c_ulonglong * 2), ("Index", wintypes.ULONG), ("Size", wintypes.ULONG),
                ("ModBase", ctypes.c_ulonglong), ("Flags", wintypes.ULONG), ("Value", ctypes.c_ulonglong),
                ("Address", ctypes.c_ulonglong), ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG),
                ("Tag", wintypes.ULONG), ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG),
                ("Name", wintypes.WCHAR * 1)]


class IMAGEHLP_MODULEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("BaseOfImage", ctypes.c_ulonglong),
                ("ImageSize", wintypes.DWORD), ("TimeDateStamp", wintypes.DWORD), ("CheckSum", wintypes.DWORD),
                ("NumSyms", wintypes.DWORD), ("SymType", wintypes.DWORD), ("ModuleName", wintypes.WCHAR * 32),
                ("ImageName", wintypes.WCHAR * 256), ("LoadedImageName", wintypes.WCHAR * 256),
                ("LoadedPdbName", wintypes.WCHAR * 256), ("CVSig", wintypes.DWORD),
                ("CVData", wintypes.WCHAR * 780), ("PdbSig", wintypes.DWORD), ("PdbSig70", ctypes.c_byte * 16),
                ("PdbAge", wintypes.DWORD), ("PdbUnmatched", wintypes.BOOL), ("DbgUnmatched", wintypes.BOOL),
                ("LineNumbers", wintypes.BOOL), ("GlobalSymbols", wintypes.BOOL), ("TypeInfo", wintypes.BOOL),
                ("SourceIndexed", wintypes.BOOL), ("Publics", wintypes.BOOL), ("MachineType", wintypes.DWORD),
                ("Reserved", wintypes.DWORD)]


SYM_TYPES = {0: "none", 1: "coff", 2: "cv", 3: "pdb", 4: "export", 5: "deferred", 6: "sym", 7: "dia", 8: "virtual"}
MAX_NAME = 2000


class Symbolizer:
    """Resolves addresses of one traced process with DbgHelp, inline frames included."""

    def __init__(self, dbghelp: Path, search_path: str):
        self.dll = ctypes.WinDLL(str(dbghelp))
        d = self.dll
        d.SymSetOptions.argtypes = [wintypes.DWORD]
        d.SymSetOptions.restype = wintypes.DWORD
        d.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        d.SymInitializeW.restype = wintypes.BOOL
        d.SymCleanup.argtypes = [wintypes.HANDLE]
        d.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                       ctypes.c_ulonglong, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        d.SymLoadModuleExW.restype = ctypes.c_ulonglong
        d.SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                                   ctypes.c_void_p]
        d.SymFromAddrW.restype = wintypes.BOOL
        d.SymAddrIncludeInlineTrace.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong]
        d.SymAddrIncludeInlineTrace.restype = wintypes.DWORD
        d.SymQueryInlineTrace.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, wintypes.DWORD, ctypes.c_ulonglong,
                                          ctypes.c_ulonglong, ctypes.POINTER(wintypes.DWORD),
                                          ctypes.POINTER(wintypes.DWORD)]
        d.SymQueryInlineTrace.restype = wintypes.BOOL
        d.SymFromInlineContextW.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, wintypes.ULONG,
                                            ctypes.POINTER(ctypes.c_ulonglong), ctypes.c_void_p]
        d.SymFromInlineContextW.restype = wintypes.BOOL
        d.SymGetModuleInfoW64.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(IMAGEHLP_MODULEW64)]
        d.SymGetModuleInfoW64.restype = wintypes.BOOL
        # UNDNAME | DEFERRED_LOADS | FAIL_CRITICAL_ERRORS | AUTO_PUBLICS | NO_PROMPTS
        d.SymSetOptions(0x2 | 0x4 | 0x200 | 0x10000 | 0x80000)
        self.handle = wintypes.HANDLE(0xB111E7)  # any unique value; no live process is inspected
        if not d.SymInitializeW(self.handle, search_path, False):
            raise OSError(f"SymInitialize failed ({ctypes.get_last_error()})")
        self.buffer = ctypes.create_string_buffer(ctypes.sizeof(SYMBOL_INFOW) + 2 * MAX_NAME)
        self.symbol = ctypes.cast(self.buffer, ctypes.POINTER(SYMBOL_INFOW)).contents
        self.name_offset = SYMBOL_INFOW.Name.offset
        self.modules: list[tuple[int, int, str, bool]] = []  # (base, end, name, loaded), sorted by base
        self.bases: list[int] = []

    def close(self) -> None:
        self.dll.SymCleanup(self.handle)

    def add_modules(self, modules: list[tuple[int, int, str]], load) -> None:
        for base, end, path in sorted(modules):
            name = Path(path).name
            loaded = False
            if load(base, path):
                loaded = bool(self.dll.SymLoadModuleExW(self.handle, None, path, None, base, end - base, None, 0))
            self.modules.append((base, end, name, loaded))
        self.modules.sort()
        self.bases = [m[0] for m in self.modules]

    def module_types(self) -> dict[str, str]:
        types = {}
        for base, _, name, loaded in self.modules:
            if not loaded:
                continue
            info = IMAGEHLP_MODULEW64()
            info.SizeOfStruct = ctypes.sizeof(IMAGEHLP_MODULEW64)
            if self.dll.SymGetModuleInfoW64(self.handle, base, ctypes.byref(info)):
                types[name] = SYM_TYPES.get(info.SymType, str(info.SymType))
        return types

    def _name(self) -> str:
        length = min(self.symbol.NameLen, MAX_NAME - 1)
        return ctypes.wstring_at(ctypes.addressof(self.symbol) + self.name_offset, length)

    def _prepare(self) -> None:
        ctypes.memset(self.buffer, 0, ctypes.sizeof(SYMBOL_INFOW))
        self.symbol.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW)
        self.symbol.MaxNameLen = MAX_NAME

    def resolve(self, address: int) -> list[tuple[str, str, bool]]:
        """Frames at this code address, innermost inline frame first: (module, function, inline)."""
        index = bisect.bisect_right(self.bases, address) - 1
        if index < 0 or address >= self.modules[index][1]:
            return [("?", "kernel" if address >= KERNEL_SPACE else "unknown", False)]
        base, _, module, loaded = self.modules[index]
        if not loaded:
            return [(module, f"+0x{address - base:x}", False)]
        frames = []
        displacement = ctypes.c_ulonglong(0)
        count = self.dll.SymAddrIncludeInlineTrace(self.handle, address)
        if count:
            context, frame_index = wintypes.DWORD(0), wintypes.DWORD(0)
            if self.dll.SymQueryInlineTrace(self.handle, address, 0, address, address, ctypes.byref(context),
                                            ctypes.byref(frame_index)):
                for i in range(count):
                    self._prepare()
                    if self.dll.SymFromInlineContextW(self.handle, address, context.value + i,
                                                      ctypes.byref(displacement), self.buffer):
                        frames.append((module, self._name(), True))
        self._prepare()
        if self.dll.SymFromAddrW(self.handle, address, ctypes.byref(displacement), self.buffer):
            frames.append((module, self._name(), False))
        else:
            frames.append((module, f"+0x{address - base:x}", False))
        return frames


# --------------------------------------------------------------------------- trace parsing

PID_RE = re.compile(r"\(\s*(\d+)\)\s*$")


def field_pid(field: str) -> int:
    match = PID_RE.search(field)
    return int(match.group(1)) if match else -1


def read_trace(xperf: Path, etl: Path, pid: int) -> dict:
    """Streams 'xperf -a dumper' and keeps the target process's samples, stacks, threads and images."""
    threads: set[int] = set()
    modules: dict[int, tuple[int, int, str]] = {}
    kernel_modules: dict[int, tuple[int, int, str]] = {}
    samples: dict[tuple[int, int], tuple[int, str]] = {}  # (time us, thread) -> (cpu, ip)
    stacks: dict[tuple[int, int], list[str]] = {}         # (time us, thread) -> frame addresses, leaf first
    current_key, current = None, None
    process = subprocess.Popen([str(xperf), "-i", str(etl), "-a", "dumper"], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace",
                               bufsize=1 << 20)
    assert process.stdout is not None
    in_header = True
    for line in process.stdout:
        if in_header:
            # The dump starts with one column-title line per event type.
            in_header = not line.startswith("EndHeader")
            continue
        head = line[:24].strip()
        if head.startswith("Stack,"):
            parts = line.split(",", 5)
            tid = int(parts[2])
            if tid not in threads:
                continue
            key = (int(parts[1]), tid)
            if key != current_key:
                # A sample's kernel and user parts can arrive as two stack events.
                current_key, current = key, stacks.setdefault(key, [])
            current.append(parts[4].strip())
        elif head.startswith("SampledProfile,"):
            parts = line.split(",", 7)
            if field_pid(parts[2]) == pid:
                samples[(int(parts[1]), int(parts[3]))] = (int(parts[5]), parts[4].strip())
        elif head.startswith(("T-Start,", "T-DCStart,")):
            parts = line.split(",", 4)
            if field_pid(parts[2]) == pid:
                threads.add(int(parts[3]))
        elif head.startswith(("I-Start,", "I-DCStart,")):
            parts = [p.strip() for p in line.split(",")]
            owner = field_pid(parts[2])
            if owner in (pid, 0):
                entry = (int(parts[3], 16), int(parts[4], 16), parts[8].strip('"'))
                (modules if owner == pid else kernel_modules)[entry[0]] = entry
    process.wait()
    return {"threads": threads, "modules": list(modules.values()), "kernel_modules": list(kernel_modules.values()),
            "samples": samples, "stacks": stacks}


def symbolize(trace: dict, search_path: str) -> dict:
    """Turns raw samples into time-ordered rows [time us, thread, cpu, stack id] over a symbol table."""
    symbolizer = Symbolizer(DBGHELP, search_path)
    try:
        kernel = [m for m in trace["kernel_modules"] if Path(m[2]).name.lower() == "ntoskrnl.exe"]
        others = [m for m in trace["kernel_modules"] if m not in kernel]
        symbolizer.add_modules(trace["modules"] + kernel, lambda base, path: Path(path).exists())
        symbolizer.add_modules(others, lambda base, path: False)

        symbols: list[tuple[str, str, bool]] = []
        symbol_ids: dict[tuple[str, str, bool], int] = {}
        resolved: dict[tuple[str, bool], tuple[int, ...]] = {}

        def frames_of(address_text: str, is_return: bool) -> tuple[int, ...]:
            key = (address_text, is_return)
            ids = resolved.get(key)
            if ids is None:
                address = int(address_text, 16) - (1 if is_return else 0)
                ids = []
                for frame in symbolizer.resolve(address):
                    if frame not in symbol_ids:
                        symbol_ids[frame] = len(symbols)
                        symbols.append(frame)
                    ids.append(symbol_ids[frame])
                ids = resolved[key] = tuple(ids)
            return ids

        stacks: list[tuple[int, ...]] = []
        stack_ids: dict[tuple[int, ...], int] = {}
        rows = []
        missing = 0
        for (time_us, tid), (cpu, ip) in sorted(trace["samples"].items()):
            raw = trace["stacks"].get((time_us, tid))
            if not raw:
                missing += 1
                raw = [ip]
            expanded: list[int] = []
            for depth, address in enumerate(raw):
                expanded.extend(frames_of(address, depth > 0))
            stack = tuple(expanded)
            if stack not in stack_ids:
                stack_ids[stack] = len(stacks)
                stacks.append(stack)
            rows.append([time_us, tid, cpu, stack_ids[stack]])
        return {"symbols": [list(s) for s in symbols], "stacks": [list(s) for s in stacks], "samples": rows,
                "samples_without_stack": missing, "symbol_types": symbolizer.module_types()}
    finally:
        symbolizer.close()


# --------------------------------------------------------------------------- record

def symbol_search_path(args: argparse.Namespace) -> str:
    parts = [str(bench.REPO / "build" / "win64" / "vc14" / "bin"), str(Path(args.harness).parent),
             str(Path(args.deps_bin)), f"srv*{SYMBOL_CACHE}*https://msdl.microsoft.com/download/symbols"]
    return ";".join(parts)


def harness_pids() -> set[int]:
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq billet-bench.exe", "/FO", "CSV", "/NH"],
                            capture_output=True, text=True).stdout
    return {int(row.split('","')[1]) for row in output.splitlines() if row.startswith('"billet-bench.exe"')}


def wait_for_exit(pid: int, timeout_s: float) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return  # already gone
    try:
        if kernel32.WaitForSingleObject(handle, int(timeout_s * 1000)) != 0:
            raise TimeoutError(f"harness process {pid} still running after {timeout_s:.0f} s")
    finally:
        kernel32.CloseHandle(handle)


def baseline_median(job: dict) -> float | None:
    if not BASELINE.exists():
        return None
    for result in json.loads(BASELINE.read_text(encoding="utf-8"))["jobs"]:
        if (result["job"], result["model"], result["threads"]) == (job["job"], job["model"], job["threads"]):
            return result.get("median_s")
    return None


def plan_one(args: argparse.Namespace, env: dict) -> dict:
    planning = SimpleNamespace(jobs=args.job, models=args.model, threads=args.threads, manifest=args.manifest,
                               harness=args.harness, cache_workers=1)
    manifest = bench.load_manifest(Path(args.manifest))
    jobs = bench.plan_jobs(planning, manifest, bench.scripted_parts(planning, env))
    if len(jobs) != 1:
        raise SystemExit(f"Expected one job for --job {args.job} --model {args.model} --threads {args.threads}, "
                         f"found {len(jobs)} (skipped in the manifest, or missing parameters?)")
    job = jobs[0]
    if manifest is not None and job["op"] not in (bench.REBUILD, "step-read"):
        model = next(m for m in manifest["models"] if m["id"] == job["model"])
        cached = bench.prepare_cache([model], planning, env)
        selection = job["params"].get("select", "all")
        if (job["model"], selection) in cached:
            job["input"] = cached[(job["model"], selection)]
            if selection != "all":
                job["params"] = {**job["params"], "select": "all", "cached_selection": selection}
    return job


def choose_runs(args: argparse.Namespace, job: dict) -> tuple[int, int]:
    """Warm-up and run counts: the benchmark's 2 + 5 by default, more runs for short jobs so the profile
    holds at least --min-seconds of operation time, fewer for long jobs (at most --max-seconds)."""
    if args.runs is not None:
        return args.warmup, args.runs
    median = baseline_median(job)
    warmup, runs = args.warmup, 5
    if median:
        if median * (warmup + runs) < args.min_seconds:
            runs = min(500, math.ceil(args.min_seconds / median) - warmup)
        elif median * (warmup + runs) > args.max_seconds:
            runs = max(1, int(args.max_seconds / median) - warmup)
            if runs == 1 and median * (warmup + 1) > args.max_seconds:
                warmup = max(0, int(args.max_seconds / median) - 1)
    return warmup, runs


def command_record(args: argparse.Namespace) -> int:
    for tool in (COLLECTOR, XPERF, DBGHELP, Path(args.harness)):
        if not tool.exists():
            log(f"Not found: {tool}")
            return 1
    env = bench.harness_env(args)
    job = plan_one(args, env)
    warmup, runs = choose_runs(args, job)
    out_dir = Path(args.out_dir) if args.out_dir else PROFILES / (args.label or datetime.date.today().isoformat())
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{job['model'].replace(':', '-')}.{job['job']}.{'mt' if job['threads'] == 'all' else 'st'}"
    work = out_dir / f"_{stem}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    report = work / "report.json"
    command = ["run", "--op", job["op"], "--model", job["harness_model"], "--threads", job["threads"],
               "--warmup", str(warmup), "--runs", str(runs), "--cpu", str(args.cpu), "--scratch", str(work),
               "--out", str(report), "--no-validity"]
    for key, value in job["params"].items():
        command += ["--param", f"{key}={value}"]
    if job.get("input"):
        command += ["--input", str(job["input"])]
    if job["threads"] == "all":
        command.append("--pin-main-thread")
    log(f"{stem}: warmup {warmup}, runs {runs}, {args.rate} Hz")
    if args.dry_run:
        log(f"{args.harness} {subprocess.list2cmdline(command)}")
        return 0

    config = work / "cpu.json"
    config.write_text(json.dumps({"Agents": [{**CPU_AGENT, "Config": {"cpuSampleRate": args.rate}}]}),
                      encoding="utf-8")
    session = str(100 + os.getpid() % 100)
    trace = work / "trace.diagsession"
    before = harness_pids()
    started = time.monotonic()
    start = subprocess.run([str(COLLECTOR), "start", session, f"/launch:{args.harness}",
                            f"/launchArgs:{subprocess.list2cmdline(command)}", f"/loadConfig:{config}",
                            f"/scratchLocation:{work}"], env=env, capture_output=True, text=True)
    try:
        if start.returncode != 0:
            log(f"Collector failed to start: {start.stdout.strip()} {start.stderr.strip()}")
            return 1
        pid = None
        while pid is None and time.monotonic() - started < 30:
            new = harness_pids() - before
            if new:
                pid = new.pop()
            elif report.exists():
                break
            else:
                time.sleep(0.05)
        if pid is not None:
            wait_for_exit(pid, args.timeout)
    finally:
        stop = subprocess.run([str(COLLECTOR), "stop", session, f"/output:{trace}"], capture_output=True, text=True)
    elapsed = time.monotonic() - started
    if not report.exists():
        log(f"The harness wrote no report (pid {pid}). Collector: {stop.stdout.strip()[-300:]}")
        return 1
    harness_report = json.loads(report.read_text(encoding="utf-8"))
    if harness_report.get("status") != "ok":
        log(f"Job failed: {harness_report.get('error')}")
        return 1
    subprocess.run([str(COLLECTOR), "expandDiagSession", str(trace)], capture_output=True, text=True)
    etls = sorted(work.glob("trace/**/*.etl"), key=lambda p: p.stat().st_size, reverse=True)
    if not etls:
        log("No ETL file in the collector's output")
        return 1
    if pid is None:
        log("Could not find the harness process")
        return 1

    parsed = read_trace(XPERF, etls[0], pid)
    profile = symbolize(parsed, symbol_search_path(args))
    document = {
        "schema_version": 1, "kind": "profile", "profiler_version": PROFILER_VERSION,
        "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "git": bench.git_info(), "job": {**{k: job[k] for k in ("job", "op", "model", "threads", "params")},
                                         "warmup": warmup, "runs": runs},
        "rate_hz": args.rate, "pid": pid, "collector_wall_s": round(elapsed, 2),
        "harness": {k: harness_report.get(k) for k in ("times_s", "median_s", "cpu_user_s", "cpu_kernel_s",
                                                       "page_faults", "memory", "details")},
        **profile,
    }
    target = out_dir / f"{stem}.json.gz"
    with gzip.open(target, "wt", encoding="utf-8") as handle:
        json.dump(document, handle, separators=(",", ":"))
    pdb_missing = sorted(name for name, kind in profile["symbol_types"].items()
                         if kind not in ("pdb", "deferred") and name.lower().startswith(("tk", "billet-bench")))
    log(f"{stem}: {len(profile['samples'])} samples ({profile['samples_without_stack']} without stack), "
        f"median {harness_report['median_s']:.4f} s, collected in {elapsed:.0f} s -> {target}")
    if pdb_missing:
        log(f"  warning: no PDB symbols for {', '.join(pdb_missing)}")
    if not args.keep_trace:
        shutil.rmtree(work, ignore_errors=True)
    return 0


# --------------------------------------------------------------------------- analysis

RUN_FRAME = re.compile(r"Operation(<.*>)?::run$")
ALLOC_NTDLL = re.compile(r"^(Rtl\w*Heap\w*|Rtlp\w*(Heap|Lfh|Hp|FreeBlock|FindEntry|Coalesce|DeCommit|Decommit|Collect|"
                         r"Allocate|Free|SecMem)\w*|RtlpInterlocked\w*|memset\w*)$")
ALLOC_CRT = {"malloc", "calloc", "free", "realloc", "malloc_base", "calloc_base", "free_base", "realloc_base",
             "msize", "msize_base", "operator new", "operator delete", "aligned_malloc", "aligned_free"}
ALLOC_OCCT = re.compile(r"^(Standard::(Allocate\w*|Free\w*|Reallocate\w*)|Standard_MMgr\w*::\w+|"
                        r"Standard_Transient::operator (new|delete)|NCollection_BaseAllocator::(Allocate|Free)\w*|"
                        r"operator (new|delete)(\[\])?)$")

HEAP, KERNEL = ("[heap]", "malloc/free and Windows heap"), ("[kernel]", "kernel (page faults, system calls)")
TBB = ("[tbb]", "TBB workers waiting for work (spinning, yielding)")
# Wrappers that say nothing about the algorithm phase: thread pools, parallel loops, public API classes.
GENERIC_FRAME = re.compile(r"^(OSD_|BOPTools_Parallel|BRep\w*API_|std::|tbb::|<>::operator\(\))")


class Profile:
    def __init__(self, path: Path):
        with gzip.open(path, "rt", encoding="utf-8") as handle:
            data = json.load(handle)
        self.path = path
        self.data = data
        self.symbols = [tuple(s) for s in data["symbols"]]
        self.stacks = [tuple(s) for s in data["stacks"]]
        self.rate = data["rate_hz"]
        self.name = f"{data['job']['model']}"
        run_ids = {i for i, (module, name, _) in enumerate(self.symbols)
                   if module.lower() == "billet-bench.exe" and RUN_FRAME.search(name)}
        self.in_run_stack = [any(f in run_ids for f in stack) for stack in self.stacks]
        rows = data["samples"]
        counts = collections.Counter(row[1] for row in rows if self.in_run_stack[row[3]])
        self.main_tid = counts.most_common(1)[0][0] if counts else None
        intervals, current = [], None
        for time_us, tid, _, stack in rows:
            if tid != self.main_tid:
                continue
            if self.in_run_stack[stack]:
                if current is None:
                    current = [time_us, time_us]
                else:
                    current[1] = time_us
            elif current is not None:
                intervals.append(current)
                current = None
        if current is not None:
            intervals.append(current)
        self.intervals = intervals
        starts = [i[0] for i in intervals]
        period = 1e6 / self.rate
        self.region = []
        for row in rows:
            time_us, tid, _, stack = row
            if tid == self.main_tid:
                if self.in_run_stack[stack]:
                    self.region.append(row)
                continue
            k = bisect.bisect_right(starts, time_us) - 1
            if k >= 0 and time_us <= intervals[k][1] + period:
                self.region.append(row)
        self.wall_us = sum(end - start + period for start, end in intervals)

    # frames, leaf first, with the harness frames (run() and above) cut off
    def frames(self, stack_id: int) -> list[tuple[str, str, bool]]:
        out = []
        for f in self.stacks[stack_id]:
            module, name, inline = self.symbols[f]
            if module.lower() == "billet-bench.exe" and RUN_FRAME.search(name):
                break
            out.append((module, name, inline))
        return out


def is_kernel(frame) -> bool:
    module = frame[0].lower()
    return (module == "?" and frame[1] == "kernel") or module.endswith(".sys") or module == "ntoskrnl.exe"


def is_alloc(frame) -> bool:
    module, name = frame[0].lower(), frame[1]
    if module == "ntdll.dll":
        return bool(ALLOC_NTDLL.match(name.lstrip("_")))
    if module in ("ucrtbase.dll", "vcruntime140.dll", "msvcp140.dll"):
        return name.lstrip("_") in ALLOC_CRT or name.lstrip("_").startswith("heap")
    if module == "tbbmalloc.dll":
        return True
    return bool(ALLOC_OCCT.match(name))


def is_scheduler(frames) -> bool:
    """True when these frames are a TBB worker waiting for work (yielding, sleeping or spinning)."""
    return any(f[0].lower() == "tbb12.dll" for f in frames) or any(
        f[1] in ("SwitchToThread", "RtlDelayExecution", "SleepEx", "Sleep") for f in frames[:2])


def short(name: str, limit: int = 90) -> str:
    """Drops template arguments and parameter lists so long C++ names stay readable."""
    out, depth = [], 0
    for ch in name:
        if ch == "<":
            if depth == 0:
                out.append("<>")
            depth += 1
        elif ch == ">" and depth:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    text = "".join(out).replace("`anonymous namespace'::", "")
    return text if len(text) <= limit else text[:limit - 1] + "…"


def attribute(frames: list, fold: bool, inline: bool):
    """Returns (self key, index of the frame where callers start) for one sample's frames."""
    i = 0
    if not inline:
        while i < len(frames) and frames[i][2]:
            i += 1
        if i == len(frames):
            i = 0
    if fold:
        j = 0
        while j < len(frames) and is_kernel(frames[j]):
            j += 1
        if j > 0:
            # Skip the system call stub (NtFreeVirtualMemory and the like), then see who called it.
            while j < len(frames) and frames[j][0].lower() == "ntdll.dll" and frames[j][1].startswith(("Nt", "Zw")):
                j += 1
            if is_scheduler(frames[j:j + 6]):
                return TBB, j
            k = j
            while k < len(frames) and is_alloc(frames[k]):
                k += 1
            return (HEAP if k > j else KERNEL), k
        if frames[0][0].lower() == "ntdll.dll" and frames[0][1].startswith(("Nt", "Zw")) and is_scheduler(frames[1:7]):
            return TBB, 1
        if is_alloc(frames[i]) or is_alloc(frames[0]):
            k = 0
            while k < len(frames) and is_alloc(frames[k]):
                k += 1
            return HEAP, k
        if frames[i][0].lower() == "tbb12.dll":
            return TBB, i + 1
    module, name, _ = frames[i]
    return (name, module), i + 1


def caller_path(frames: list, start: int, depth: int) -> tuple:
    callers = []
    for module, name, inline in frames[start:]:
        if inline or is_alloc((module, name, inline)):
            continue
        key = short(name, 60)
        if not callers or callers[-1] != key:
            callers.append(key)
        if len(callers) >= depth:
            break
    return tuple(callers)


def analyze(profile: Profile, fold: bool, inline: bool, path_depth: int) -> dict:
    total = len(profile.region)
    self_counts = collections.Counter()
    incl_counts = collections.Counter()
    modules = collections.Counter()
    paths = collections.defaultdict(collections.Counter)
    ancestors = collections.defaultdict(collections.Counter)  # key -> caller function -> samples
    sites = collections.Counter()  # allocation sites: first caller outside the allocator
    threads = collections.Counter()
    # Concurrency: 1 ms buckets inside the runs, and the threads seen in each.
    bucket_threads = collections.defaultdict(set)
    keys_by_sample = []
    for time_us, tid, _, stack in profile.region:
        threads[tid] += 1
        bucket_threads[time_us // 1000].add(tid)
        frames = profile.frames(stack)
        if not frames:
            keys_by_sample.append(None)
            continue
        key, start = attribute(frames, fold, inline)
        keys_by_sample.append(key)
        self_counts[key] += 1
        modules[key[0] if key in (HEAP, KERNEL, TBB) else key[1]] += 1
        path = caller_path(frames, start, path_depth)
        paths[key][path] += 1
        ancestors[key].update({short(name, 60) for module, name, is_inline in frames[start:]
                               if not is_inline and module.lower() != "billet-bench.exe"
                               and not is_alloc((module, name, is_inline))})
        if key == HEAP:
            sites[path[:1]] += 1
        seen = set()
        for module, name, is_inline in frames:
            if is_inline and not inline:
                continue
            k = (name, module)
            if k not in seen:
                seen.add(k)
                incl_counts[k] += 1
        if fold and any(is_alloc(f) for f in frames):
            incl_counts[HEAP] += 1
        if fold and is_kernel(frames[0]):
            incl_counts[KERNEL] += 1
    buckets = 0
    serial_buckets = 0
    for start_us, end_us in profile.intervals:
        for b in range(int(start_us) // 1000, int(end_us) // 1000 + 1):
            buckets += 1
            if len(bucket_threads.get(b, ())) <= 1:
                serial_buckets += 1
    serial_self = collections.Counter()
    for (time_us, tid, _, _), key in zip(profile.region, keys_by_sample):
        if key is not None and len(bucket_threads[time_us // 1000]) <= 1:
            serial_self[key] += 1
    return {"total": total, "self": self_counts, "incl": incl_counts, "paths": paths, "sites": sites,
            "ancestors": ancestors,
            "modules": modules, "threads": threads, "wall_s": profile.wall_us / 1e6,
            "active": total / (profile.wall_us / 1e6 * profile.rate) if profile.wall_us else 0.0,
            "serial_share": serial_buckets / buckets if buckets else 0.0, "serial_self": serial_self}


def command_report(args: argparse.Namespace) -> int:
    paths = []
    for pattern in args.profiles:
        matches = sorted(glob.glob(pattern))
        if not matches:
            log(f"No profile matches {pattern}")
            return 1
        paths.extend(matches)
    profiles = [Profile(Path(p)) for p in paths]
    results = [analyze(p, not args.no_fold, args.inline, args.path_depth) for p in profiles]
    job = profiles[0].data["job"]
    print(f"### {job['job']} ({'all cores' if job['threads'] == 'all' else 'single thread'}), "
          f"{len(profiles)} model(s)\n")
    print("| Model | Median (s) | Runs profiled | Samples in runs | Active threads | Serial time |")
    print("| --- | --- | --- | --- | --- | --- |")
    for p, r in zip(profiles, results):
        print(f"| {p.name} | {p.data['harness']['median_s']:.4f} | {p.data['job']['warmup']}+{p.data['job']['runs']} "
              f"| {r['total']} | {r['active']:.2f} | {100 * r['serial_share']:.0f}% |")
    print()

    def mean_share(counter_name: str, key) -> tuple[float, list[float]]:
        shares = [r[counter_name].get(key, 0) / r["total"] if r["total"] else 0.0 for r in results]
        return sum(shares) / len(shares), shares

    def top_merged(counter_name: str, count: int) -> list[tuple[object, float]]:
        merged = collections.Counter()
        for r in results:
            n = sum(r[counter_name].values())
            for key, c in r[counter_name].items():
                merged[key] += c / n / len(results) if n else 0
        return merged.most_common(count)

    print("Time by module: " + ", ".join(f"{name} {100 * share:.0f}%" for name, share in top_merged("modules", 8)))
    if any(r["sites"] for r in results):
        print("\nAllocation sites (share of heap time): " + ", ".join(
            f"`{short(site[0], 60) if site else '?'}` {100 * share:.0f}%" for site, share in top_merged("sites", 6)))
    print()

    keys = set()
    for r in results:
        keys.update(r["self"])
    ranked = sorted(keys, key=lambda k: -mean_share("self", k)[0])[:args.top]
    # Mean inclusive share by (shortened) function name, to name the phase a hotspot belongs to.
    phase_share = collections.Counter()
    for r in results:
        for (name, module), c in r["incl"].items():
            if module.lower() != "billet-bench.exe" and r["total"]:
                phase_share[short(name, 60)] += c / r["total"] / len(results)
    print("| # | Function | Module | Self | Incl. | Self per model | Main call path (share of its samples); phase |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for rank, key in enumerate(ranked, 1):
        self_mean, self_shares = mean_share("self", key)
        incl_mean, _ = mean_share("incl", key)
        merged = collections.Counter()
        coverage = collections.Counter()
        present = 0
        for r in results:
            n = sum(r["paths"][key].values())
            for path, c in r["paths"][key].items():
                merged[path] += c / n if n else 0
            if n:
                present += 1
                for caller, c in r["ancestors"][key].items():
                    coverage[caller] += c / n
        weight = sum(merged.values())
        best, best_w = merged.most_common(1)[0] if merged else ((), 0)
        path_text = " ← ".join(best) if best else "—"
        # Phase: the most specific caller that holds at least 10% of all time and lies on the stack of
        # at least half of this function's samples.
        phases = [(phase_share[c], c) for c, cov in coverage.items()
                  if present and cov / present >= 0.5 and phase_share[c] >= 0.10 and c not in best
                  and not GENERIC_FRAME.match(c)]
        phase_text = f"; in `{min(phases)[1]}`" if phases else ""
        name, module = key
        print(f"| {rank} | `{short(name)}` | {module} | {100 * self_mean:.1f}% | {100 * incl_mean:.1f}% | "
              f"{' / '.join(f'{100 * s:.1f}' for s in self_shares)} | {path_text} "
              f"({100 * best_w / weight if weight else 0:.0f}%){phase_text} |")
    print()
    if args.inclusive:
        keys = set()
        for r in results:
            keys.update(k for k in r["incl"] if k[1].lower() != "billet-bench.exe")
        ranked = sorted(keys, key=lambda k: -mean_share("incl", k)[0])[:args.inclusive]
        print("| Function | Module | Inclusive | Self |")
        print("| --- | --- | --- | --- |")
        for key in ranked:
            name, module = key
            print(f"| `{short(name)}` | {module} | {100 * mean_share('incl', key)[0]:.1f}% | "
                  f"{100 * mean_share('self', key)[0]:.1f}% |")
        print()
    if args.callers:
        # Call paths, inline frames included, of samples whose stack contains the named function.
        print(f"Call paths through `{args.callers}` (share of all samples):\n")
        merged = collections.Counter()
        for p in profiles:
            for _, _, _, stack in p.region:
                frames = p.frames(stack)
                hit = next((i for i, f in enumerate(frames) if args.callers in f[1]), None)
                if hit is None:
                    continue
                below = tuple(short(f[1], 50) + ("*" if f[2] else "") for f in frames[max(0, hit - 2):hit])
                above = tuple(short(f[1], 50) + ("*" if f[2] else "") for f in frames[hit + 1:hit + 1 + args.path_depth])
                merged[(below, short(frames[hit][1], 50), above)] += 1 / len(p.region) / len(profiles)
        for (below, name, above), share in merged.most_common(12):
            print(f"- {100 * share:.2f}%: {' → '.join(below[::-1]) + ' → ' if below else ''}**{name}** "
                  f"← {' ← '.join(above)}")
        print("\n(* inlined)\n")
    if job["threads"] == "all":
        print("Self time while only one thread ran (share of all samples): " + ", ".join(
            f"`{short(key[0], 60)}` {100 * share:.1f}%" for key, share in
            ((k, mean_share("serial_self", k)[0]) for k, _ in top_merged("serial_self", 8))))
        print()
    return 0


# --------------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    record = sub.add_parser("record", help="profile one job")
    record.add_argument("--job", required=True, help=f"job type: {', '.join(bench.ALL_JOBS)}")
    record.add_argument("--model", required=True, help="model id, or part:<id> for rebuild")
    record.add_argument("--threads", default="1", choices=bench.THREAD_MODES)
    record.add_argument("--warmup", type=int, default=2)
    record.add_argument("--runs", type=int, help="timed runs (default: 5, adjusted by --min/--max-seconds)")
    record.add_argument("--min-seconds", type=float, default=10.0,
                        help="add runs until the profile holds this much operation time (from the baseline)")
    record.add_argument("--max-seconds", type=float, default=90.0, help="cap on profiled operation time")
    record.add_argument("--rate", type=int, default=4000, help="samples per second (default: 4000)")
    record.add_argument("--cpu", type=int, default=max(0, (os.cpu_count() or 2) - 2))
    record.add_argument("--label", help="profile folder name (default: today's date)")
    record.add_argument("--out-dir", help=f"profile folder (default: {PROFILES}/<label>)")
    record.add_argument("--keep-trace", action="store_true", help="keep the collector's trace files")
    record.add_argument("--timeout", type=float, default=3600.0)
    record.add_argument("--dry-run", action="store_true")
    record.add_argument("--manifest", default=str(bench.MANIFEST))
    record.add_argument("--harness", default=str(bench.DEFAULT_HARNESS))
    record.add_argument("--install", default=str(bench.DEFAULT_INSTALL))
    record.add_argument("--deps-bin", default=str(bench.DEFAULT_DEPS_BIN))

    report = sub.add_parser("report", help="print hotspots of saved profiles")
    report.add_argument("profiles", nargs="+", help="profile files or glob patterns (same job type)")
    report.add_argument("--top", type=int, default=10)
    report.add_argument("--inclusive", type=int, default=0, help="also list this many functions by inclusive time")
    report.add_argument("--inline", action="store_true", help="attribute self time to inlined functions")
    report.add_argument("--no-fold", action="store_true", help="do not fold heap and kernel frames")
    report.add_argument("--path-depth", type=int, default=4, help="callers shown per call path")
    report.add_argument("--callers", help="also list the call paths through functions whose name contains this")

    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    return command_record(args) if args.command == "record" else command_report(args)


if __name__ == "__main__":
    sys.exit(main())
