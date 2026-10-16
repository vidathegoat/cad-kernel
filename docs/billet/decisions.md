# Decisions

Short record of design decisions and why, oldest first. Open questions, including those from SPEC.md section 10, are listed at the end until they are answered.

## 2026-10-12: Base Billet on tag V8_0_1 through a new `main` branch

- **Decision:** `main` was created from the upstream tag `V8_0_1` (commit `b8f597c677`) and made the default branch on GitHub. `master` is left unchanged as a mirror of upstream `master`.
- **Why:** The GitHub fork started from upstream `master` (8.1.0.dev1, 42 commits past 8.0.1). A new branch avoided rewriting a published one. The fork had no commits of its own, so nothing needed to be carried over.
- **Remotes:** `upstream` points to https://github.com/Open-Cascade-SAS/OCCT. The fork's parent on GitHub is `dpasukhi/OCCT`, a maintainer's fork without the release tags, so tags and merges come from `upstream`, not from GitHub's "Sync fork".

## 2026-10-12: Build configuration for 8.0.1

Configure command (Windows, from the repository folder):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_GENERATOR_INSTANCE=C:/Program Files/Microsoft Visual Studio/2022/Community" -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DBUILD_USE_VCPKG=ON -DINSTALL_DIR=C:/billet-install -DINSTALL_TEST_CASES=ON -DUSE_TBB=ON -DUSE_RAPIDJSON=ON
```

- All option names in SPEC.md 4.3 are valid for 8.0.1. Three options were added:
  - `BUILD_USE_VCPKG=ON`: upstream CI sets it. Without it, OCCT stores OFF in the cache, and a later re-configure silently leaves vcpkg mode.
  - `INSTALL_TEST_CASES=ON`: installs `tests/` so the DRAW test grid can run from the install folder (SPEC.md 4.4). It is off by default.
  - `CMAKE_GENERATOR_INSTANCE`: pins the Visual Studio 2022 edition, because the maintainer's desktop also has VS 2022 Build Tools and VS 2026.
- FreeType and Tcl/Tk need no flags. `USE_FREETYPE` and `USE_TK` are on by default, and Tcl is required by DRAW.
- vcpkg installs the dependencies from OCCT's own manifest (`adm/vcpkg/ports/opencascade`, registry baseline 2025-10-17, triplet `x64-windows`): FreeType 2.13.3, Tcl/Tk 8.6.16 (OCCT's overlay port), oneTBB 2022.2.0 and RapidJSON 2025-02-26.
- `BUILD_OPT_PROFILE` stays at upstream's `Default` for now (see open questions).
- The install folder's scripts load the third-party DLLs from `build/vcpkg_installed/x64-windows/bin`, so the build folder must be kept.
- Baseline build (unmodified 8.0.1, MSVC 14.44): 0 compiler and linker warnings. Configure prints one upstream warning, that the Tcl and Tk DLL folders were not found. It is harmless because those DLLs sit in the same vcpkg folder as FreeType and TBB, which is on the path.

## 2026-10-12: "Modified for Billet." marker in files that are not C++

- SPEC.md section 2 asks for `// Modified for Billet.` near the header of each modified upstream file. Where `//` is not a comment, the same text is written in that file's comment syntax: `# Modified for Billet.` in `.gitignore`, `<!-- Modified for Billet. -->` in Markdown.
- In C++ files, the line goes after the license header, separated by blank lines, so the header block itself stays unchanged (first used on 2026-10-15 in `ChFi3d.cxx`).

## 2026-10-12: `.gitignore` allows Billet's paths

- Upstream's `.gitignore` ignores everything at the repository root except a listed set. Billet adds `SPEC.md`, `NOTICE.md`, `bench/` and `docs/` to that list, and ignores `bench/corpus/` (SPEC.md section 3).

## 2026-10-12: Test grid baseline setup

- **Test data:** the public pack upstream CI uses for 8.0.1, `opencascade-dataset-7.9.0.zip` (SHA-256 `0d574a2347e0ab780cdfeac8addfe744e58be0f30effb970c7e14dcd80b44578`), extracted to `C:/billet-testdata` and passed through `CSF_TestDataPath`. Tests whose files are not in the pack are skipped (7,831 of 21,006).
- **How it runs:** from the install folder after `env.bat`, as `DRAWEXE -v -c testgrid -outdir <folder> -parallel 16`. This is the same as typing `testgrid -parallel 16` in the `draw.bat` console, except that `-v` keeps viewer windows off screen, as in upstream CI. A full run takes about 10 minutes on the maintainer's desktop.
- **Results:** the summary, failing tests and skipped tests are in `bench/results/testgrid-baseline.json`. The full logs stay outside the repository in `C:/billet-results/testgrid-baseline`.
- **Baseline failures:** 560 are known upstream failures (tests marked TODO). All 295 unexpected failures are in visualization groups and come from features this configuration does not build: OpenGL ES (270), image input/output without FreeImage (23) and the Direct3D host (1). One more is a Tcl file channel error in `caf basic W12`. None are in modeling groups.

## 2026-10-12: Benchmark harness and runner (Phase 1)

- **Harness:** `billet-bench`, C++17, built against the installed Billet in `build-bench/`. One process runs one job (operation x model x thread mode), so peak memory is measured per job. Usage is in `bench/harness/README.md`.
- **Protocol:** setup is not timed (load the STEP file, select solids, build tool shapes). Then 1 warm-up run and 5 timed runs, and the median is reported. Peak working set and peak commit come from the OS after the timed runs, before the fingerprint is computed.
- **Thread modes:** `1` turns off the parallel flags, routes `OSD_Parallel` to a one-thread OCCT pool and pins the process to logical CPU 2. `all` turns the parallel flags on for booleans, meshing and BRepCheck, with TBB scheduling.
- **Rebuild parts:** eight scripted parts: the OCCT tutorial bottle, an L-bracket, a shelled enclosure, a revolved flange, a heatsink with 48 fillets, a drilled manifold block, a gearbox-style housing (ribs, bearing bosses, bores, vent pattern) and a spoked wheel (pocket pattern with filleted edges). Edges and faces are selected by geometry, not index.
- **Job choices:** booleans use the model against a copy translated by 10%, 7% and 5% of its bounding box. Mesh presets: coarse is 0.1% of the bounding box diagonal and 0.5 rad, fine is 0.02% and 0.2 rad. Fillet and shell parameters are set per model in the manifest. `billet-bench probe` finds them.
- **Fingerprint:** volume, area, bounding box (without triangulation), topology counts, triangle and node counts, and BRepCheck validity of the output. Outputs are saved as BREP (with triangulation only for meshing), or STEP for `step-write` with a fixed header time stamp. The runner records their SHA-256.
- **Runner:** `bench/runner/billet_bench.py`, Python 3 standard library only, with `fetch`, `run` and `compare`. Jobs run at high process priority. Results go to `bench/results/<date>-<commit>.json`.
- **Code style:** the harness is formatted with the repository's `.clang-format` (clang-format 19.1.5 from Visual Studio 2022).

## 2026-10-12: Verification rule for serial changes (amends SPEC.md 7.1)

- **Decision:** serial changes must keep topology counts and BRepCheck validity identical, and volume, area and bounding box within a relative 1e-12. Output file hashes are recorded but need not match. SPEC.md 7.1 is amended accordingly. `compare --rule serial` (default) and `--rule parallel` (1e-9) apply the two rules.
- **Why:** upstream OCCT is not bit-identical between two runs of the same build, even single-threaded. In the first harness test, the bottle part's volume differed by about 1e-15 (relative), and two parts' BREP files differed in bytes. Shape maps are hashed by memory address, so processing order follows the heap layout. Any allocation change, Phase 2's first category, moves the heap too, so byte identity would reject correct changes.

## 2026-10-12: Build profile for benchmarks

- **Decision:** benchmarks use upstream's `BUILD_OPT_PROFILE=Default`, the same build as Phase 0.
- **Why:** it gives faster rebuilds in the optimization loop and clearer profiler call stacks than `Production` (`/GL`, `/LTCG`). Release builds may use `Production` later, measured separately.

## 2026-10-12: Timing noise measures

- **Power plan:** a Windows power plan named "Billet benchmark" was created on the maintainer's desktop. It is a copy of High performance with processor boost disabled. The runner activates it for the duration of a run and restores the previous plan afterwards (`--power-plan ''` turns this off). With boost off, the spread across 8 runs of an all-cores boolean job fell from 8.6% to 4.5%. Single-threaded jobs stayed within 1.4 to 3.9%. Absolute times are about 10 to 15% slower.
- **Main thread pinning:** in all-cores mode the runner pins the calling thread to the same CPU as single-threaded jobs, leaving worker threads free. The heatsink rebuild's spread fell from 10.7% to 2.5%.
- **Remaining:** very short all-cores jobs (under about 50 ms) remain noisy. The noise check in SPEC.md 5.5 is judged on the real corpus.

## 2026-10-12: Benchmark corpus (SPEC.md 10.1)

- **Sources:** a research pass verified 60 public STEP models. Each URL is pinned to an immutable commit or record, answers with HTTP 200 and Content-Length, and starts with an ISO-10303-21 header. The models come from open hardware and data projects: Voron, Creality Ender 3, Jubilee, Framework, System76 Thelio and Launch, Luxonis, Precious Plastic, ODRI, SO-ARM101, Thor, Prusa, Adafruit, FreeCAD library, KiCad library, NIST MBE PMI models and Zenodo.
- **Selection:** 33 of the 58 redistributable candidates: 15 large (over 20 MB), 11 mid (1 to 20 MB) and 7 small, 582 MB in total. That is about half, a third and the rest, as SPEC.md 5.1 asks. They come from seven CAD systems and use AP203, AP214 and AP242. The stress cases include a perforated plate with thousands of holes, gears, a knurled nut, sheet metal, draft angles and thin organic shells.
- **Excluded:**
  - two non-commercial models (Ultimaker 2, Valve Index);
  - the OCP RMC tray, whose Open Web Foundation CLA needs legal review;
  - assemblies over 60 MB, because one 241 MB assembly alone would add about 45 minutes to every run;
  - for cost, a few further mid-size and small models, and assemblies that duplicate a product already in the set.
- **Not committed:** every model is downloaded by `fetch` and verified by SHA-256 into `bench/corpus/`. Fifteen URLs are GitHub LFS media links, which have bandwidth limits, so a mirror may be needed later. Licenses are GPL-2.0, GPL-3.0, CC-BY-3.0/4.0, CC-BY-SA-4.0, MIT, Apache-2.0, BSD-3-Clause and NIST public domain. The license of each model is recorded in the manifest.
- **Per-model jobs** come from one single-threaded scouting pass:
  - Assemblies run booleans, fillet and shell on their largest solid.
  - Fillet edge sets and shell faces were found with `billet-bench probe` at 0.3% of the model's bounding box diagonal.
  - A job that failed was skipped. Fillet or shell over 20 s per run was skipped, and so was a fine mesh over 60 s.
  - Booleans use a copy translated by 10/7/5% of the bounding box. Where that took over 10 s per run, 50/35/25% is used instead. All three booleans are kept up to 10 s per run, only `bool-cut` up to 60 s, and none above.
  - The skipped jobs are listed per model in the manifest (`skip`).
- **Upstream failures found:** fuse and cut fail on four NIST parts with a translated copy of themselves, and on CTC-02 every boolean runs past 5 minutes. These are recorded as skips, not fixed (robustness is a non-goal).
- **Inputs:** every job except `step-read` loads a binary BREP copy of its model (`bench/corpus/_cache`), made once per model. Binary BREP stores doubles exactly, so results are bit-identical to reading the STEP file; text BREP rounds to 15 digits and changed a fillet result in the 12th digit. The copy is made once and reused across builds, so later builds measure operations on exactly the same input.
- **Cost:** about 3.4 hours for a full run (612 jobs, both thread modes) on the maintainer's desktop. `run --dry-run` shows the plan, and `--jobs`, `--models` and `--threads` run a subset.

## 2026-10-13: Timing noise, second round (SPEC.md 5.5)

- **First noise check failed:** two full runs of unmodified 8.0.1 on 2026-10-12, one in the evening and one at night, agreed within 5% on only 363 of 612 jobs. Per job type, geometric means agreed within 3%, except `rebuild` (15% single-threaded, 9% all cores). The differences clustered in time: during the maintainer's evening use, around Windows' 2 AM maintenance window, and at the start of the first run.
- **Pinned CPU:** single-threaded jobs moved from logical CPU 2 to the first CPU of the last core (CPU 14 on the maintainer's Ryzen 7 7800X3D). Windows runs most background threads and interrupts on the first cores: with the machine idle, CPUs 0 to 3 were 22 to 34% busy and CPUs 12 to 15 about 2%. On CPU 14, corpus jobs ran 9 to 13% faster, and the spread between processes fell from 4–9% to 1.4–4.4%.
- **SMT sibling:** a busy sibling CPU slows a pinned single-threaded job by 25 to 48%. While a job runs, Windows moves background threads onto its sibling, so background load reaches single-threaded jobs mainly that way.
- **Background load:** the desktop's always-on applications keep about 1 to 2 CPUs busy, and the maintainer keeps them running. The runner guards against the load instead:
  - At the start of the run, it takes the idle level as the median of five 1-second load samples.
  - Before each job, it waits until the load is back near the idle level, for up to 5 minutes. This also lets the previous job's process teardown finish.
  - Over the timed runs, the harness measures the CPU time not used by its own process (other processes, interrupts and DPCs) and how busy the pinned CPU's sibling was.
  - A job is rerun, up to twice, if other work kept more than 1.5 CPUs busy above the idle level, or if the sibling was busy more than 25% of the time. Jobs still disturbed after that are flagged in the results.
  - Windows counts CPU time in 15.6 ms ticks, so jobs whose timed runs total under a second rely on the wait alone.
- **Input cache:** the cached BREP inputs no longer depend on the harness version. Harness changes do not rebuild them, so every build keeps measuring the same input.

## 2026-10-13: Noise gate per job type (amends SPEC.md 5.5)

- **Decision:** two runs of the same build must agree within 2% on the geometric mean of every job type, per thread mode (`compare --max-type-change 2`). Per-job changes are reported for information.
- **Why:** SPEC.md 7.1 judges speed per operation (a 3% gain on the targeted one, no other slower by more than 2%), so the noise must be below 2% at that level. Per job, 5% is out of reach with the desktop's background applications running. In a subset check (142 jobs, two runs back to back), 90% of jobs agreed within 5%, and 20 of 22 job types within 2%. The rest of the per-job noise comes from short jobs and from per-process effects. Repeating short operations within a run, to time at least 0.2 s, did not reduce it.

## 2026-10-13: Two warm-up runs (amends SPEC.md 5.3)

- **Decision:** the median of 5 timed runs is taken after 2 warm-up runs instead of 1.
- **Why:** in larger jobs (STEP read, shell, mesh), the first two runs in a process are 8 to 10% faster than every later run. Page faults and kernel time stay the same; user time rises as the heap fills. With one warm-up, the first timed run fell in that fast phase. The housing rebuild shows no such effect. A full run takes about 15% longer.

## 2026-10-13: One fingerprint tolerance, 1e-9 (amends SPEC.md 7.1)

- **Decision:** for every change, serial or parallel, volume, area and bounding box must agree within a relative 1e-9, and topology counts and validity must match exactly. `compare` defaults to 1e-9, and its `--rule` option was removed.
- **Why:** the 1e-12 serial rule rejected unmodified 8.0.1. Over 20 to 40 processes of the same build, the housing part's volume varied by up to 5e-10 relative and its area by up to 8.5e-11. Its bounding box and topology never changed. The bottle part varies by about 1e-14. The other six parts were bit-identical, and so were 604 of 612 jobs between the two full runs.

## 2026-10-14: Baseline results (SPEC.md 5.5)

- **Runs:** `bench/results/baseline-8.0.1.json` and `noise-check-8.0.1.json` are two full runs of unmodified 8.0.1, back to back on 2026-10-14 from 02:20 to 09:40, with the desktop's always-on applications running. All 612 jobs succeeded in both. Each run took about 3 hours 40 minutes.
- **Noise gate passed:** the geometric means of all 22 job types, per thread mode, agree within 2%. The largest differences are `rebuild` all cores (-1.6%) and `mesh-coarse` all cores (-1.0%). The other 20 are within 0.9%.
- **Per job, for information:** 95% of jobs agree within 5% and 80% within 2%, with a median difference of 0.8%. The 30 jobs that differ by more than 5% all have medians under 0.6 s, 25 of them under 0.1 s.
- **Fingerprints:** all 612 jobs agree within 1e-9, with identical topology counts and validity.
- **Load guard:** in the baseline run, one job was rerun and none stayed disturbed. During the noise check, Windows Update installed the monthly updates from 07:22 to about 07:40, and its installer and the search indexer kept about 1.5 CPUs busy. Four jobs were rerun then and one later. Two stayed flagged as disturbed; they differ from the baseline by +3.2% and +1.7%.

## 2026-10-14: PDB files in the Release build (Phase 2)

- **Decision:** the Release build also writes PDB files, so that profiles show function names. The configure command in this file gains five flags:

  ```
  "-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /Ob2 /DNDEBUG /Zi" "-DCMAKE_C_FLAGS_RELEASE=/MD /O2 /Ob2 /DNDEBUG /Zi" "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" "-DCMAKE_MODULE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF"
  ```

  The harness does the same in `bench/harness/CMakeLists.txt`.
- **Why not upstream's option:** `BUILD_FORCE_RelWithDebInfo` also adds `/Ob1`, which inlines less, so the profiled code would differ from the benchmarked code. `/Zi` does not change code generation. `/DEBUG` turns off `/OPT:REF` and `/OPT:ICF` by default, so both are restated.
- **Code unchanged:** compared with the binaries the baseline was measured with, every DLL's code section has the same size and the same instructions. The only differing bytes, in every DLL and executable, are RIP-relative addresses of read-only data, which moved by 104 to 240 bytes because the PDB path is stored in `.rdata`. In two DRAW plugin DLLs, `.data` also moved by one page. The baseline results therefore remain valid. The build took 9.5 minutes with no warnings. The previous binaries are kept in `C:/billet-results/binaries/8.0.1-release-nopdb`.

## 2026-10-14: Profiler (SPEC.md 6)

- **Tool:** Visual Studio 2022's Performance Profiler, through its command-line collector `VSDiagnostics.exe` with the CPU sampling agent at 4,000 samples per second with call stacks. It runs without administrator rights. Intel VTune is not installed.
- **Script:** `bench/runner/billet_profile.py record` runs one benchmark job under the collector, with the same job parameters and input as the runner. `xperf` (Windows Performance Toolkit) turns the trace into text, and DbgHelp (Debugging Tools for Windows) resolves the harness process's frames, inline functions included. Microsoft's symbol server provides the system DLL symbols, cached in `C:/billet-symbols`. `report` prints hotspots of one or more saved profiles. Profiles are saved in `C:/billet-results/profiles`.
- **What counts:** only samples inside the operation's `run()`: on the main thread, samples whose stack is inside it, and on other threads, samples taken while it runs. Setup, `prepareRun()` and the fingerprint are excluded.
- **Runs:** 2 warm-up and 5 timed runs as in the benchmark, with more timed runs for short jobs (at least 10 s of operation time, about 40,000 samples) and fewer for long ones (at most 90 s). The warm-up runs are profiled too.
- **Shares:** with several models, a function's share is the mean of its share per model, so each model counts equally, as in the benchmark's geometric means. Time in `malloc`, `calloc`, `free` and the Windows heap (`ucrtbase.dll`, `ntdll.dll`) is reported as one entry, `[heap]`, together with the kernel time it causes; other kernel time is `[kernel]`, and TBB workers waiting for work are `[tbb]`.
- **Phase 2 run:** 66 profiles on 2026-10-14, 53 single-threaded (3 to 5 models per job type) and 13 on all cores, in 87 minutes with the desktop in use. The results are in `docs/billet/perf-log.md`; the profiles (100 MB) stay in `C:/billet-results/profiles/phase2`. The symbol cache in `C:/billet-symbols` holds 4.7 GB, mostly system PDBs fetched by a first trial with `xperf -symbols`; it can be deleted, and the profiler then downloads only what it needs.

## 2026-10-15: Test grid tests that fail at random

- **Finding:** upstream marks some known failures as random with `TODO ?` (19 test files). Such a test passes or fails from run to run on the same build. `offset compshape A3` gave the correct result 12 times and a faulty one 8 times out of 20 runs, both with unmodified 8.0.1 and with the first Phase 3 change.
- **Decision:** a status change of such a test does not by itself count as a regression or as an output change (SPEC.md 7.1). It is checked by running the test repeatedly with both builds, or by showing that the test does not reach the changed code.

## 2026-10-16: Speed check after a Windows update (amends SPEC.md 7.1)

- **Finding:** `baseline-8.0.1.json` was measured on 2026-10-14 from 02:20. Later that morning, Windows installed the October cumulative update (KB5127648, OS build 26200.9711), a .NET update, two Realtek audio drivers and a Defender platform update, and restarted at 10:49. Since then, unmodified 8.0.1 runs slower on the maintainer's desktop. Single-threaded jobs that take at least a second run 4 to 7% longer in STEP read and write, check and booleans, and 0 to 5% longer in meshing. All-cores jobs vary more, and short jobs change less. Over the 240 jobs measured again on 2026-10-16, the geometric means per job type and thread mode moved by −1.9 to +4.8%. Memory integrity (VBS) was on before and after the restart, and the benchmark power plan is unchanged.
- **Effect:** against the baseline, the full run of the first Phase 3 change showed STEP read 3.3 to 3.4% slower and check 2.2 to 2.5% slower. Both exceed the 2% limit of SPEC.md 7.1, although neither operation reaches the changed code.
- **Decision:** when the machine has changed since the latest accepted results (a Windows, driver or firmware update), the speed check compares the change with the previous build timed on the same night. A subset of models runs twice, back to back: first with the previous build's DLLs, then with the new ones. For the first change, it was 12 models and the 8 rebuild parts (240 jobs). The change's full run, measured after the update, becomes the latest accepted results for the next cycle.
- **Before a timing run:** compare the Windows Update history with the date of the latest accepted results. `Get-HotFix` does not list drivers; the history in Settings, or `Microsoft.Update.Session` in PowerShell, does.

## Open questions

1. Whether the first release ships prebuilt binaries or source only (SPEC.md 10.2).
2. Whether optimizations are offered upstream, and when (SPEC.md 10.3).
3. Whether Billet eventually adds its own API layer for the CAD app (SPEC.md 10.4).
