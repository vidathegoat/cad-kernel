# Billet — Development Spec

Billet is a performance fork of Open CASCADE Technology (OCCT) 8.0.1. Its goal is to make OCCT measurably faster without changing its results, so that open CAD applications can be built on a faster kernel.

This spec is written for Claude Code and for the maintainer. Read it fully before starting any phase.

---

## Product direction

Billet exists to power a future CAD application. These decisions about that app set Billet's priorities:

| Decision | Choice | Effect on Billet |
| --- | --- | --- |
| Target user | Professional assemblies first; a hobbyist project type later if feasible | Corpus and benchmarks weighted toward large assemblies |
| Platform | Native desktop | No WebAssembly target; optimize for multi-core desktop CPUs |
| Modeling style | Sketch and feature tree, matching the Fusion 360 and SolidWorks workflow; a separate mesh-modeling tab (Blender style) is a later goal | Feature rebuild speed is the top metric; mesh modeling is out of Billet's scope |
| Speed priority | Modeling speed first, file import second | Modeling operations are optimized before STEP import |

**Separate projects.** Billet and the CAD application are separate repositories. Billet is an LGPL library that anyone can use and redistribute. The app has its own repository and license and links Billet dynamically, which keeps the LGPL obligations limited to Billet itself.

---

## 1. Ground rules

1. **Same input, same output.** Phases 1 through 4 change speed and memory only, never geometric results. A change that alters output is rejected, however much faster it is.
2. **Stay drop-in compatible.** Billet keeps OCCT's public API, ABI, class names and namespaces for the 8.0.x line. Any project that builds against OCCT 8.0.1 must build against Billet with no source changes.
3. **Stay mergeable.** Keep diffs against upstream small and local. Do not reformat, rename or reorganize upstream files. This keeps upstream merges cheap and keeps patches offerable to Open Cascade.
4. **Measure before and after.** No optimization is merged without benchmark numbers from the harness in section 5.
5. **Claude Code never commits.** At each commit point, Claude Code stops, summarizes the change and proposes a commit message in the format from section 9. The maintainer makes every commit manually.

### Non-goals (for now)

- Robustness fixes that change behavior (boolean or fillet tolerance handling).
- New modeling features.
- API additions or removals.
- The CAD application, which lives in its own repository and links Billet as a library.
- Mesh (Blender-style) modeling, which belongs to the app and a mesh library such as Manifold.

---

## 2. Licensing and branding

OCCT is licensed under LGPL 2.1 with the Open CASCADE Exception 1.0. Billet inherits that license.

Required in the repository at all times:

- `LICENSE_LGPL_21.txt` and `OCCT_LGPL_EXCEPTION.txt`, unchanged from upstream.
- All upstream copyright headers, unchanged.
- A new `NOTICE.md` at the repository root stating that Billet is a modified version of Open CASCADE Technology, naming the upstream version it is based on, and linking to the upstream repository.
- Every modified upstream file gets one added comment line near its header: `// Modified for Billet.` No other header changes.

Branding:

- "Billet" is used for the repository, README, packages, release names and documentation.
- "Open CASCADE" and "OCCT" are trademarks of Open Cascade SAS. Use them only to describe origin ("based on Open CASCADE Technology 8.0.1"), never as the product name.
- Library file names, class names and namespaces stay as upstream (rule 2).

---

## 3. Repository layout

Upstream files stay where they are. Billet adds only these paths:

```
SPEC.md                  this file
NOTICE.md                fork notice (section 2)
README.md                Billet section added at the top; upstream text kept below it
bench/
  harness/               C++ benchmark harness (links against the built libraries)
  runner/                script that runs the harness and writes results
  corpus.manifest.json   list of benchmark models: source URL, license, checksum
  corpus/                downloaded models — gitignored, never committed
  results/               baseline and run results (JSON, committed)
docs/billet/
  perf-log.md            one entry per optimization: hotspot, change, before/after
  decisions.md           short record of design decisions and why
  upstream-merges.md     log of each upstream merge and any conflicts
```

---

## 4. Phase 0 — Setup and baseline build

**Goal:** a clean Billet build on the maintainer's desktop with the upstream test suite run and recorded.

### 4.1 Prerequisites (desktop, Windows)

- Visual Studio 2022 with the "Desktop development with C++" workload
- CMake 3.20 or newer
- Git (GitHub Desktop is used for commits, but Git must also be on PATH for CMake and vcpkg)
- vcpkg, for third-party dependencies: FreeType, Tcl/Tk (required for the DRAW test harness), oneTBB, RapidJSON

### 4.2 Upstream remote

If the fork was made through GitHub with "contribute to the parent project" selected, GitHub Desktop already tracks upstream. Otherwise add it once. Run on the **desktop, in PowerShell, inside the repository folder**:

```powershell
git remote add upstream https://github.com/Open-Cascade-SAS/OCCT.git
```

```powershell
git fetch upstream --tags
```

### 4.3 Build

Claude Code must confirm every CMake option name against the 8.0.1 CMake files before running these. Option names below are a starting point, not verified for 8.0.1.

Run on the **desktop, in Developer PowerShell for VS 2022, inside the repository folder**:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DINSTALL_DIR=C:/billet-install -DUSE_TBB=ON -DUSE_RAPIDJSON=ON
```

```powershell
cmake --build build --config Release --parallel
```

```powershell
cmake --install build --config Release
```

### 4.4 Upstream test suite baseline

OCCT's regression tests run inside the DRAW harness. Launch DRAW from the install folder, then run the full test grid inside it.

Run on the **desktop, in PowerShell, inside `C:/billet-install`**:

```powershell
.\draw.bat
```

Then, **inside the DRAW console** that opens:

```
testgrid -parallel 16
```

Record the summary (passed, failed, skipped) and the list of failing tests in `bench/results/testgrid-baseline.json`. Pre-existing failures are not Billet's to fix in this phase; they become the comparison baseline.

### 4.5 Phase 0 done when

- Release build succeeds with no new warnings beyond upstream's.
- `NOTICE.md`, README section and `docs/billet/` files exist.
- Test grid baseline is recorded.

**Commit point:** setup files and baseline results.

---

## 5. Phase 1 — Benchmark harness and corpus

**Goal:** a repeatable benchmark that times the operations that matter and saves their outputs for comparison.

### 5.1 Corpus

- 30 to 60 real STEP models, weighted toward professional work: about half mid-size to large assemblies (over 20 MB), a third mid-size parts (1 to 20 MB), and the rest small parts (under 1 MB).
- Each model is listed in `corpus.manifest.json` with its source URL, license and SHA-256 checksum.
- Only models whose license allows redistribution may be committed. All others are downloaded by the runner into the gitignored `bench/corpus/` folder.
- Include models known to stress booleans and fillets (many small fillets, thin walls, tangent faces).

### 5.2 Operations measured

Priority order follows the product direction: feature rebuild and modeling operations first, file exchange second.

| ID | Operation | Notes |
| --- | --- | --- |
| `rebuild` | Replay of a scripted feature sequence (sketch profile, extrude, cut, fillet, chamfer, shell, pattern) | Top metric; mimics what a user feels when editing an early feature. 5 to 10 scripted parts stored in `bench/harness/` |
| `shell` | Shell (hollow) a solid with a set wall thickness | |
| `step-read` | STEP import to shape | Includes XDE (names, colors) |
| `step-write` | Shape to STEP export | |
| `bool-fuse` / `bool-cut` / `bool-common` | Boolean operations | Generated test pairs per model (model against offset or translated copy) |
| `fillet` | Constant-radius fillet on a fixed edge set | Edge set chosen per model and stored in the manifest |
| `mesh` | BRepMesh tessellation | Two deflection settings: coarse and fine |
| `check` | BRepCheck_Analyzer validity | Also used to validate outputs |

### 5.3 Metrics per run

- Wall time: median of 5 runs after 1 warm-up run.
- Peak memory.
- Thread mode: single-threaded and all cores, measured separately.

### 5.4 Output fingerprint per run

Saved alongside timings, used to prove output is unchanged:

- Volume, surface area, bounding box
- Counts of solids, faces, edges, vertices
- Mesh triangle and node counts (for `mesh`)
- BRepCheck validity result
- A serialized BREP file for exact diffing (stored in the gitignored corpus folder, hash recorded in results)

### 5.5 Phase 1 done when

- One command runs the full benchmark and writes `bench/results/<date>-<commit>.json`.
- A baseline run on unmodified 8.0.1 is saved as `bench/results/baseline-8.0.1.json`.
- Running the benchmark twice on the same build gives timings within 5% of each other. If not, fix noise (power plan, background load, CPU pinning) before Phase 2.

**Commit points:** harness, then corpus manifest, then baseline results.

---

## 6. Phase 2 — Profiling

**Goal:** a ranked list of hotspots, so optimization effort goes where time is actually spent.

- Profile each operation on 3 to 5 representative models with Visual Studio's Performance Profiler or Intel VTune (free).
- Record the top 10 hotspots per operation in `docs/billet/perf-log.md`: function, share of total time, call path, and a one-line hypothesis.
- Rank candidates by expected gain against risk. Low-risk categories, in order:
  1. Allocation churn (repeated small allocations, missing reserve, copies that could be moves)
  2. Redundant computation (recomputed values that can be cached per call)
  3. Spatial acceleration (linear searches over faces or edges that can use bounding-volume trees)
  4. Parallelizing independent per-face or per-edge work
  5. Lock contention in shared caches

**Commit point:** the hotspot list.

---

## 7. Phase 3 — Optimization loop

One hotspot per cycle. Each cycle:

1. Pick the top-ranked candidate from the perf log, favoring hotspots in `rebuild`, booleans, fillets and shells over STEP import.
2. Write down the expected change and gain in the perf log before coding.
3. Make the smallest change that addresses it.
4. Build Release.
5. Run the benchmark and compare against the latest accepted results.
6. Verify (section 7.1).
7. Record before and after numbers in the perf log.
8. Stop and propose a commit (section 9).

### 7.1 Verification (all must pass)

- **Serial-only changes:** topology counts and BRepCheck validity identical to baseline; volume, area and bounding box within a relative tolerance of 1e-12. Output file hashes (BREP, STEP) are recorded for information but need not match, because upstream OCCT is not bit-identical between runs: shape maps are hashed by memory address, so processing order follows the heap layout (see `docs/billet/decisions.md`).
- **Changes that add parallelism:** fingerprints within a relative tolerance of 1e-9, BRepCheck valid, topology counts identical. Byte-identical BREP is not required, since face ordering may change.
- **Test grid:** no test that passed in the baseline now fails.
- **Speed:** at least a 3% median gain on the targeted operation, with no other operation slower by more than 2%.

A change that fails any check is reverted, and the attempt is logged in the perf log as a negative result so it is not retried.

### 7.2 What Claude Code must not do in this phase

- Change tolerance values or algorithm choices that affect results.
- Touch public headers in ways that change ABI (adding members to public classes, changing virtual tables).
- Batch several unrelated optimizations into one change.

---

## 8. Phase 4 — Releases and upstream merges

### 8.1 Versioning

`Billet <major>.<minor>.<patch>` with the upstream base recorded in the release notes, for example "Billet 0.1.0, based on OCCT 8.0.1".

- First release once total gains are meaningful (target: at least 15% median improvement on one major operation with no regressions).
- Each release includes a summary table of gains per operation from the latest results file.

### 8.2 Upstream merges

- When Open Cascade publishes a new 8.0.x maintenance release, merge its tag into Billet.
- Rerun the full benchmark and test grid after the merge.
- Log conflicts and resolutions in `docs/billet/upstream-merges.md`.

### 8.3 Upstreaming (optional)

Optimizations that are clean and well-measured can be offered to Open Cascade as pull requests. This is a maintainer decision per change and is not required.

---

## 9. Commit guidelines

All commits are made manually by the maintainer. Claude Code proposes messages but never runs `git commit`. Commits carry no Co-Authored-By trailer or any AI attribution.

### 9.1 Format

```
type(scope): short summary in past tense

Description of what changed and why. For performance changes, include
the benchmark result and the verification performed.
```

- Summary line: lowercase type and scope, 72 characters or fewer, no trailing period.
- Blank line, then a description of one or more short paragraphs or bullets.

### 9.2 Types

| Type | Use |
| --- | --- |
| `perf` | Speed or memory improvement with unchanged output |
| `fix` | Bug fix in Billet's own code (harness, scripts) or a build fix |
| `feat` | New Billet tooling (harness features, runner options) |
| `bench` | Corpus, benchmark results or baseline updates |
| `test` | Test additions or changes |
| `build` | CMake, vcpkg or dependency changes |
| `docs` | SPEC, README, perf log, decisions |
| `chore` | Housekeeping with no functional effect |
| `merge` | Upstream merge commits |

### 9.3 Scopes

`boolean`, `fillet`, `mesh`, `step`, `check`, `core` (Foundation and shared types), `harness`, `corpus`, `build`, `docs`, `upstream`.

### 9.4 Examples

```
perf(mesh): parallelized per-face tessellation in BRepMesh

Faces are now meshed in parallel when the shape has more than 64 faces.

- mesh (fine): 412 ms -> 168 ms median across corpus, all cores
- mesh (coarse): 95 ms -> 51 ms
- Fingerprints within 1e-9, topology counts identical, BRepCheck valid
- testgrid: no new failures
```

```
bench(corpus): added 12 assembly models from public STEP sources

All 12 are listed in corpus.manifest.json with license and checksum.
Three allow redistribution and are committed; the rest download on run.
```

```
docs(perf-log): recorded phase 2 hotspots for booleans and STEP import
```

### 9.5 When to commit

- End of each phase step marked "Commit point" in this spec.
- After every accepted optimization (one optimization per commit).
- After every upstream merge, as its own commit.
- Never commit a broken build or a failing verification.

---

## 10. Open decisions

Record the answers in `docs/billet/decisions.md` when made.

1. Which models make up the initial corpus, and where they come from.
2. Whether Billet ships prebuilt binaries or source only for the first release.
3. Whether optimizations are offered upstream, and when.
4. Long-term: whether Billet stays on OCCT's API forever or eventually adds its own API layer for the CAD app.
