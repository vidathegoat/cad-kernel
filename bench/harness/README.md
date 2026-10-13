# Billet benchmark

The harness (`bench/harness`, C++) runs one benchmark job per process and reports timings, peak memory and an output fingerprint as JSON. The runner (`bench/runner/billet_bench.py`, Python 3, standard library only) downloads the corpus, runs every job and writes one results file. See SPEC.md section 5.

## Build the harness

Build and install Billet first (README.md). Then, from the repository folder:

```powershell
cmake -S bench/harness -B build-bench -G "Visual Studio 17 2022" -A x64 -DOpenCASCADE_DIR=C:/billet-install/cmake
```

```powershell
cmake --build build-bench --config Release
```

The harness links the installed DLLs at run time, so after rebuilding and reinstalling Billet the harness does not need rebuilding.

## Run

```powershell
python bench/runner/billet_bench.py fetch
```

```powershell
python bench/runner/billet_bench.py run
```

`run` writes `bench/results/<date>-<commit>.json`. Use `--label baseline-8.0.1` for a named file, and `--jobs`, `--models` or `--threads` to run a subset. `--dry-run` lists the planned jobs and the manifest's time estimate. A full run takes about 4 hours on the maintainer's desktop. Outputs (BREP, or STEP for `step-write`) go to `bench/corpus/_outputs/<label>/`, which is not committed. Their SHA-256 is recorded in the results.

Before the first job, `run` converts each model once to a binary BREP copy in `bench/corpus/_cache/`, along with the solid that assembly jobs select. Every job except `step-read` loads that copy in its untimed setup. Binary BREP keeps doubles exact, so results match reading the STEP file bit for bit. The copies are kept across harness and library builds, so every build measures the same input. Delete the folder to rebuild them after changing how `convert` selects or writes shapes.

During a run, the runner activates the Windows power plan "Billet benchmark", a copy of High performance with processor boost disabled, and restores the previous plan afterwards. If a run is killed, the next run restores the plan first. `--power-plan ''` keeps the current plan.

Background load from other applications slows jobs, so the runner guards against it. At the start it measures the machine's idle load (the median of five 1-second samples). Before each job, it waits until the load is back near that level, for up to 5 minutes (`--quiet-wait`). The harness reports the CPU time not used by its own process during the timed runs and, for single-threaded jobs, how busy the pinned CPU's SMT sibling was. A busy sibling slows a pinned job by 25 to 50%. A job is rerun, up to twice (`--retries`), when other work kept more than 1.5 CPUs busy above the idle level (`--background-margin`, or `--max-background` for an absolute limit), or when the sibling was busy more than 25% of the time (`--max-sibling`). Jobs whose timed runs total under a second are not checked, because Windows counts CPU time in 15.6 ms ticks. Jobs still disturbed after the reruns are marked `disturbed` in the results. `--no-load-guard` turns all of this off. Closing other applications still gives the steadiest timings.

```powershell
python bench/runner/billet_bench.py compare bench/results/baseline-8.0.1.json bench/results/<new>.json
```

`compare` lists the timing change per job and the geometric mean per job type, and checks fingerprints by the SPEC.md 7.1 rule: topology counts and validity must match exactly, and volume, area and bounding box must agree within a relative 1e-9 (`--tolerance` changes it). Add `--max-type-change 2` to fail when any job type's geometric mean changes by more than 2% (the SPEC.md 5.5 noise gate), `--max-change 5` to fail on any single job changing by more than 5%, and `--exact-outputs` to also require identical output files (upstream OCCT does not guarantee this between runs).

## Jobs

| Job | What is timed | Parameters (manifest) |
| --- | --- | --- |
| `rebuild` | Full replay of a scripted part (`billet-bench parts` lists them) | none |
| `step-read` | STEP file to an XDE document with names, colors, layers and properties | none |
| `step-write` | Shape to STEP file (fixed header time stamp, so output is repeatable) | `select` |
| `bool-fuse`, `bool-cut`, `bool-common` | Boolean of the model with a translated copy of itself | `select`, `translate` (fractions of the bounding box, default `0.1,0.07,0.05`) |
| `fillet` | Constant-radius fillet on a fixed edge set | `select`, `radius`, `edges` (indices) |
| `shell` | Hollow with a wall thickness, optionally opening faces | `select`, `thickness`, `faces` (indices) |
| `mesh-coarse`, `mesh-fine` | BRepMesh from a clean shape (0.1% and 0.02% of the bounding box diagonal; 0.5 and 0.2 rad) | `select` |
| `check` | BRepCheck_Analyzer | `select` |

`select` is `all`, `largest-solid` or `solid:<n>`. Indices follow `TopExp::MapShapes` order on the selected shape.

Each job runs with one setup that is not timed (load and select the model, build tool shapes), 2 warm-up runs and 5 timed runs. The median is reported. Two warm-up runs are needed because in larger jobs OCCT's first two runs in a process are 8 to 10% faster than later ones. Peak memory is the process peak after the timed runs. The fingerprint is computed from the last run's output: volume, area, bounding box, topology counts, triangle and node counts, and BRepCheck validity. For each timed run the harness also records user and kernel CPU time and page faults, and for the timed runs together the background load described above (`load`).

Thread modes: `1` turns off OCCT's parallel flags, routes `OSD_Parallel` to a one-thread pool and pins the process to one logical CPU (`--cpu`). The default is the second to last logical CPU, which is the first CPU of the last core on SMT machines, because Windows runs most background threads and interrupts on the first cores. `all` turns the parallel flags on for booleans, meshing and BRepCheck. The runner also pins the main thread of `all` jobs to the same CPU, while worker threads stay free, because an unpinned main thread made mostly-sequential jobs vary by up to 10% between runs.

## Corpus manifest

`bench/corpus.manifest.json` lists each model with `id`, `file` (name in `bench/corpus/`), `url`, `sha256`, `bytes`, `license`, `license_url`, `redistributable`, and optional `archive` (`{"member": "<path in zip>"}`), `ops` (parameters per job, for example `{"fillet": {"radius": 0.5, "edges": "3,8,12"}}`) and `skip` (jobs not to run for this model). `fillet` and `shell` run only for models that define their parameters. `fetch --update` records `sha256` and `bytes` for new entries.

To find working fillet and shell parameters for a model:

```powershell
build-bench\Release\billet-bench.exe probe --op fillet --model bench\corpus\<file> --param radius_frac=0.003 --param count=12
```

```powershell
build-bench\Release\billet-bench.exe probe --op shell --model bench\corpus\<file> --param thickness_frac=-0.003
```

Both need the Billet and vcpkg DLL folders on `PATH`, as the runner sets them: `C:\billet-install\win64\vc14\bin` and `build\vcpkg_installed\x64-windows\bin`.
