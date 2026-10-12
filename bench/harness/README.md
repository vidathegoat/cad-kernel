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

`run` writes `bench/results/<date>-<commit>.json`. Use `--label baseline-8.0.1` for a named file, and `--jobs`, `--models` or `--threads` to run a subset. Outputs (BREP, or STEP for `step-write`) go to `bench/corpus/_outputs/<label>/`, which is not committed. Their SHA-256 is recorded in the results.

```powershell
python bench/runner/billet_bench.py compare bench/results/baseline-8.0.1.json bench/results/<new>.json
```

`compare` lists the timing change per job and the geometric mean per job type, and checks fingerprints. Add `--max-change 5` to fail on any timing change over 5%, `--tolerance 1e-9` for changes that add parallelism, and `--exact-outputs` to also require identical output files.

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

Each job runs with one setup that is not timed (load and select the model, build tool shapes), 1 warm-up run and 5 timed runs. The median is reported. Peak memory is the process peak after the timed runs. The fingerprint is computed from the last run's output: volume, area, bounding box, topology counts, triangle and node counts, and BRepCheck validity.

Thread modes: `1` turns off OCCT's parallel flags, routes `OSD_Parallel` to a one-thread pool and pins the process to one logical CPU (`--cpu`, default 2). `all` turns the parallel flags on for booleans, meshing and BRepCheck.

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
