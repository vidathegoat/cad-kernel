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
- **Rebuild parts:** six scripted parts: the OCCT tutorial bottle, an L-bracket, a shelled enclosure, a revolved flange, a heatsink with 48 fillets and a drilled manifold block. Edges and faces are selected by geometry, not index.
- **Job choices:** booleans use the model against a copy translated by 10%, 7% and 5% of its bounding box. Mesh presets: coarse is 0.1% of the bounding box diagonal and 0.5 rad, fine is 0.02% and 0.2 rad. Fillet and shell parameters are set per model in the manifest. `billet-bench probe` finds them.
- **Fingerprint:** volume, area, bounding box (without triangulation), topology counts, triangle and node counts, and BRepCheck validity of the output. Outputs are saved as BREP (with triangulation only for meshing), or STEP for `step-write` with a fixed header time stamp. The runner records their SHA-256.
- **Runner:** `bench/runner/billet_bench.py`, Python 3 standard library only, with `fetch`, `run` and `compare`. Jobs run at high process priority. Results go to `bench/results/<date>-<commit>.json`.
- **Code style:** the harness is formatted with the repository's `.clang-format` (clang-format 19.1.5 from Visual Studio 2022).

## Open questions

1. Which models make up the initial corpus, and where they come from (SPEC.md 10.1).
2. Whether the first release ships prebuilt binaries or source only (SPEC.md 10.2).
3. Whether optimizations are offered upstream, and when (SPEC.md 10.3).
4. Whether Billet eventually adds its own API layer for the CAD app (SPEC.md 10.4).
5. `BUILD_OPT_PROFILE` for benchmark and release builds: upstream `Default`, or `Production` (adds `/GL`, `/LTCG` and more). Decide before the Phase 1 baseline, because switching later shifts every timing.
6. How to verify serial changes (SPEC.md 7.1). Upstream OCCT is not bit-identical between two runs of the same build, even single-threaded. In the first harness test, the bottle part's volume differed by about 1e-15 (relative), and two parts' BREP files differed in bytes. The likely cause is shape maps hashed by memory address, so processing order follows the heap layout. Any allocation change, which is Phase 2's first category, also moves the heap. So "BREP byte-identical" would reject correct changes. Proposal: for serial changes, require identical topology counts and validity, and volume, area and bounding box within a relative 1e-12. Keep the BREP hashes for information only.
7. Timing noise (SPEC.md 5.5). In the first test, single-threaded jobs repeated within 3%, but all-cores jobs of 10 to 400 ms varied by up to 10%. This needs work before Phase 1 is done.
