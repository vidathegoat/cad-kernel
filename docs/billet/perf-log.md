# Performance log

One entry per optimization attempt, newest first. Rejected attempts are logged too, so they are not retried (SPEC.md section 7.1). Phase 2 adds the ranked hotspot list below.

## Entry format

```
### YYYY-MM-DD: <operation>: <short title>

- Hotspot: function, share of total time, call path
- Expected change and gain: written before coding
- Change: what changed and where
- Before -> after: median wall time per affected operation (single-threaded and all cores), peak memory
- Verification: fingerprints, BREP diff, test grid, speed thresholds (SPEC.md 7.1)
- Result: accepted (commit) or rejected (reason)
```

## Hotspots

Phase 2 profiles, recorded on 2026-10-14 with `bench/runner/billet_profile.py` on unmodified 8.0.1 (Release build with PDB files, see `docs/billet/decisions.md`). Each operation was profiled single-threaded on 3 to 5 models from the corpus, chosen to span its range of run times, with the benchmark's job parameters and inputs. Operations with parallel code were also profiled on all cores (section "All cores").

How to read the tables:

- **Self** is the time spent in the function itself, including code inlined into it, as a share of the operation's time. It is the mean over the profiled models, so each model counts equally, as in the benchmark's geometric means. The range across models follows in brackets.
- **Incl.** is the time spent in the function and everything it calls.
- **`[heap]`** is `malloc`, `calloc`, `free` and the Windows heap (`ucrtbase.dll`, `ntdll.dll`) together, with the kernel time they cause. OCCT's `Standard::Allocate` calls `calloc` in this build (`USE_MMGR_TYPE=NATIVE`), so every allocation is also zeroed. **`[kernel]`** is other kernel time, mostly page faults.
- **Call path** gives the nearest callers, nearest first, or for `[heap]` the main allocation sites.
- **Hypothesis** names the likely cause, with the candidate (H1 to H20, see "Ranked candidates") it belongs to.
- Function names come from the PDB files. Identical functions are folded by the linker (`/OPT:ICF`), so a few names stand for several functions; for example, `BRepFilletAPI_MakeChamfer::Build` also stands for `BRepFilletAPI_MakeFillet::Build`.
- Times per run in the profiles are within about 15% of the baseline: sampling adds some overhead, processor boost was on, and the desktop was in use. Shares are hardly affected.
- The profiles are kept in `C:/billet-results/profiles/phase2` (66 files, not committed); `billet_profile.py report` reprints any table from them.

### Ranked candidates

Ranked by expected gain against risk, with modeling operations ahead of file exchange. "Share" is the part of the operation's time the candidate addresses: the mean over the profiled models, with the largest model value in brackets. Expected gains are rough estimates from those shares, to be measured in Phase 3. Categories follow SPEC.md section 6: 1 allocation churn, 2 redundant computation, 3 spatial acceleration, 4 parallelizing, 5 lock contention.

| Rank | ID | Candidate | Cat. | Share | Expected gain | Risk |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | H1 | `ChFi3d::IsTangentFaces` builds two `BRepAdaptor_Surface`s and two `BRepTopAdaptor_TopolTool`s only to read `NbSamples()`, which is a constant 100 for a plane. Building them projects every edge of a planar face onto the plane, twice. Skip them for planar faces. | 2 | fillet 31% (77%), rebuild 17% (83%), shell 7% (16%); the bottle part, profiled separately, 45% | heatsink rebuild −60 to −70%; fillet about −25% (geometric mean of the profiled models); shell −5% | Low: the same sample count, so the same loop; one static function. |
| 2 | H2 | `RefineShell` (`BOPAlgo_ShellSplitter.cxx`) finds each shell edge in its two faces by scanning the faces' edges, then scans them again for internal edges. Build one edge-to-first-occurrence map per face. | 2 | bool-common 15% (59%), other booleans 1% | rear-cover common about −50%; in the rear-cover fuse on all cores, `RefineShell` is 42% of the time and runs on one thread | Low: same first-occurrence semantics; file-local function. |
| 3 | H8 | OCCT allocates through `Standard::Allocate`, which calls `calloc` on the Windows heap in this build. Build with `USE_MMGR_TYPE=TBB`, an upstream option that uses oneTBB's scalable allocator (already a dependency) and keeps the zeroing. | 1 | heap 8 to 24% of every operation; 62 to 81% of it through `Standard::Allocate` | −3 to −9% across operations; less heap lock contention on all cores | Low for results, but heap addresses change (map orders follow them, within the 1e-9 rule) and peak memory may grow: measure both. Build change, maintainer decision. |
| 4 | H20 | On all cores, each `BRepOffset_Tool::Inter3D` call runs a tiny `BOPAlgo_PaveFiller` with parallel loops of a few items; idle TBB workers spin and yield between them (71 to 78% of all CPU samples). Run the per-pair intersection serially, or skip parallel loops below a minimum size. | 4, 5 | shell all cores: scheduler 71–78% | shell all cores from 0.7× to at least 1× the single-thread speed; the housing rebuild's 44% spin also drops | Low: single-thread and all-cores outputs already match. |
| 5 | H7 | `BRepClass3d_SolidExplorer` builds a face intersector, with a sampled polyhedron, for every face of the solid, even when only one classification follows (`BOPTools_AlgoTools::IsInvertedSolid`). Build intersectors on first use and reject faces whose box the ray misses. | 2, 3 | bool-fuse 18% (49%), bool-cut 8% (20%), rebuild 4% (8%), STEP read 4% (11%), check 2% (6%) | gear-plate fuse −40%; fuse about −10%; cut −5% | Medium: the explorer keeps state between calls (`myFirstFace`, `myParamOnEdge`) and `Intersector()` is const; the box test must bound the face with its tolerance. |
| 6 | H4 | `CSLib_Class2d::SiDans` scans the polygon once with ON detection and four more times for the tolerance corners. Count all four corners' crossings in one pass. | 2 | bool-fuse 4% (21%), bool-common 6% (23%), bool-cut 4% (16%), fillet 4% (17%), meshing 1–4% | keyboard fuse and common −10%; shredder cut −8% | Low: the same arithmetic per edge; no new members (`CSLib_Class2d` is a public class). |
| 7 | H3 | `BOPAlgo_PaveFiller::PerformFF`: for every face pair with a curved face, all edge pairs are visited, and `IsClosedFF` runs for both edges inside the inner loop. Compute each edge's index and closed flag once per face pair, keeping the loop order. | 2 | booleans 3–4% (12%), serial | keyboard −9%; booleans −2 to −3%; also on all cores | Low: same order, same values. |
| 8 | H5 | `occ::down_cast` uses MSVC's `dynamic_cast`, which compares type names when the class lives in another DLL; failing casts are the slowest (a plane tested as `Geom_RectangularTrimmedSurface`). Add an exact-type check (`DynamicType() == STANDARD_TYPE(...)`) before the cast at the hottest sites: `BRep_Tool::CurveOnPlane`, `GeomBndLib_Curve2d`, `StepData_StepWriter::Send`. | 2 | 1 to 5% everywhere (heatsink 11%) | −1 to −4%; heatsink −8% | Low: falls back to the cast when the type differs. |
| 9 | H6 | `BSplCLib_CacheParams::PeriodicNormalization` is not inlined and saves two vector registers before its non-periodic early return, on every B-spline evaluation. Inline the non-periodic path. | 2 | 2 to 4% in booleans, fillet, shell, check and STEP read | −1.5 to −3% | Very low: inline function body in a header, no layout change. |
| 10 | H9 | The Delaunay circle grid (`NCollection_CellFilter` in `BRepMesh_CircleTool`) has about n^0.3 cells per direction, so cells hold long circle lists; adding a circle builds a temporary key per covered cell (`memcpy`, MurmurHash). Size the grid from the expected triangle count, and insert without temporary keys. | 3 | mesh-coarse 35% (76%), mesh-fine 39% (56%) | meshing −15 to −30%; framework-16 coarse about −50% | Medium: a lookup must return the same circles in the same order whatever the cell size (it looks so: one cell per point, lists in insertion order); verify. |
| 11 | H10 | `BRepMesh_Delaun` creates several `NCollection_IncAllocator`s with 1 MB first blocks per face, which Windows commits and releases each time. Reuse them per thread. | 1 | mesh-coarse 11% (15%), mesh-fine 5% (6%) | meshing −4 to −9% | Low to medium: upstream chose to return the memory to the system; peak memory must stay close. |
| 12 | H12 | `IntCurveSurface_HInter` builds a new `Adaptor3d_TopolTool`, samples the B-spline face and builds a polyhedron for every edge it intersects with that face. Sample each face once. | 2 | bool-cut 7% (25%), fillet 7% (36%) | shredder cut −20%; wedge-plate fillet −30% | Medium: the cache needs a home without changing public class layouts. |
| 13 | H13 | `math_FunctionSetRoot::SearchDirection` creates a `math_SVD` at every Newton step; each of its matrices zero-fills a fixed 64-element buffer even for a 3×4 system. Reuse the objects across steps. | 1 | rebuild 3% (13%), booleans 1–2% (4%) | wheel rebuild −3% | Low: same arithmetic. |
| 14 | H11 | `BOPAlgo_PaveFiller::GetStickVertices` rebuilds a map of both faces' sub-shapes and scans every interference, for each face pair. Index the interferences by shape once. | 2 | booleans 1% (5%) | keyboard −4% | Low to medium: insertions must keep their order. |
| 15 | H15 | `BRepCheck_Wire::SelfIntersect` and `IntersectWires` build both pcurves' polygons again for every edge pair. Build each polygon once. | 2 | check 19% (34%) | check −8 to −15% | Medium: the polygons are built inside `Geom2dInt_GInter`. |
| 16 | H16 | `BRep_Tool::CurveOnPlane` copies the edge's 3D curve, projects it and allocates several adaptors on every call; it is hot in fillet (30%), rebuild (16%), check (16%) and STEP read (9%). Make the call cheaper without changing the projection. | 1 | as listed (H1 and H5 remove part) | −3 to −8% after H1 and H5 | Medium: the projected pcurve must stay bit-identical. |
| 17 | H14 | Shell: every face pair in `BRepOffset_Tool::Inter3D` gets a new `IntTools_Context`, so each face's 2D classifier is rebuilt per pair (`IntTools_FClass2d::Init`). Make the classifier's construction cheaper (fewer allocations), or share contexts. | 1, 2 | shell 12% (33%) | shell −3% (allocation only) to −25% (shared contexts) | Low for allocations; high for sharing, because face tolerances change between runs. |
| 18 | H18 | `ShapeFix_SplitCommonVertex`, run before writing STEP, compares every vertex of every wire with every vertex of every other wire of the face, walking the topology again each time. Keep a vertex set per wire and skip wire pairs that share no vertex. | 2 | step-write 17% (48%) | Thelio write −45%; step-write about −15% | Low: pairs without a shared vertex change nothing. File export. |
| 19 | H17 | `Extrema_GenExtPS::BuildGrid` computes, for each projected point, the distance to every node, edge and cell of the sample grid, with a surface evaluation per grid edge. Skip edges and cells that cannot beat the current best. | 3 | step-read 12% (29%) | slow STEP imports −10 to −20% | Medium: ties must resolve as before. File import. |
| 20 | H19 | `StepData_StepWriter::Send` appends small pieces to a string that is reallocated at every append. Build lines without reallocating. | 1 | step-write: string reallocation about 3% of the time, heap 24% | step-write −3 to −5% | Low. File export. |

Not ranked: the global optimizers and particle swarms in extrema (`math_PSO`, `math_GlobOptMin`), the B-spline span caches rebuilt by scattered evaluation, and `BRepMesh_DataStructureOfDelaun::cleanLink`. Faster versions would change algorithms or public class layouts.

The Phase 2 lead that OCCT slows down over successive runs in one process ("heap aging") is not confirmed: in the profiles, which ran with processor boost on, warm-up and timed runs of booleans, shell, meshing and STEP read differ by less than 4% in time and not at all in heap and kernel shares.

### rebuild

Five of the eight scripted parts: heatsink (0.45 s per run), housing (0.28 s), wheel (0.11 s), manifold (0.09 s) and enclosure (0.05 s). Booleans take 73% of the time (83 to 94% in four parts). The heatsink spends 83% in `ChFi3d::IsTangentFaces`, which its 48 fillets call while chaining edges and building corners.

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 17.0% (8.6–31.2) | 18.0% | 81% through `Standard::Allocate` (`calloc`). Heatsink: geometry copies in `BRep_Tool::CurveOnPlane`; booleans: `IntSurf_LineOn2S` point sequences, 2D classifiers | Allocation churn. Most of the heatsink's 31% disappears with H1; the rest is spread and needs a faster allocator (H8). |
| 2 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 3.5% (0.2–7.7) | 5.9% | `Extrema_GlobOptFuncCQuadric::value` ← `math_PSO` … edge/face extrema in `IntTools_FaceFace` | Particle-swarm global minimization evaluates the curve once per particle and iteration. The algorithm stays; only per-call overhead can go (H6). |
| 3 | `GeomAdaptor_Surface::EvalD0` (TKG3d) | 2.9% (0.1–4.4) | 5.0% | Same `math_PSO` paths, and `GeomLib_CheckCurveOnSurface` | As above. |
| 4 | `FindSITargetTypeInstance` (vcruntime140) | 2.7% (0.8–8.7) | 2.7% | `__RTDynamicCast` ← `occ::down_cast` in `BRep_Tool::CurveOnPlane` and `GeomBndLib_Curve2d` | MSVC's `dynamic_cast` compares type names when the classes come from another DLL, and failing casts are the slowest. An exact-type check before the cast (H5). |
| 5 | `GeomInt_BSpParLeastSquare…::MakeTAA` (TKGeomAlgo) | 2.3% (0.0–6.2) | 2.7% | Least squares in `GeomInt_WLApprox` ← `IntTools_FaceFace::MakeCurve` | Approximation of intersection curves; dense matrix products per Brent step. Numeric; low priority. |
| 6 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 2.0% (0.0–7.3) | 2.0% | `BSplCLib_Cache::D0` ← `GeomAdaptor_Curve::EvalD0` | Not inlined, and it saves two vector registers before the non-periodic early return, on every B-spline evaluation (H6). |
| 7 | `SVD_Decompose` (TKMath) | 2.0% (0.0–8.8) | 2.1% | `math_SVD` ← `SearchDirection` ← `math_FunctionSetRoot` (walking in `IntPatch_ImpPrmIntersection`) | A new `math_SVD` at every Newton step when there are more unknowns than equations. Each of its matrices zero-fills a fixed 64-element buffer even for a 3×4 system, and `U` is zeroed again. Mostly the decomposition itself; reusing the objects (H13) saves the setup. |
| 8 | `math_BFGS::Perform` (TKMath) | 1.7% (0.0–6.8) | 5.1% | Gradient approximation in `GeomInt_WLApprox` (housing) | Numeric optimizer; temporaries per iteration. Low priority. |
| 9 | `~TopLoc_SListOfItemLocation` (TKBRep) | 1.7% (0.8–3.5) | 1.7% | `TopExp::MapShapesAndAncestors`, `BRepTools::UVBounds` | Location handles copied and released for every explored sub-shape. Falls with fewer repeated topology walks (H1, H3). |
| 10 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 1.7% (0.6–3.6) | 1.9% | `TopExp_Explorer::Next` ← `BRepTools::AddUVBounds` | Every `BRepAdaptor_Surface` recomputes the face's UV bounds by walking all its edges (H1). |

### bool-fuse

Five models: freecad-plastic-bottle (6.3 s per run), launch-keyboard-aluminum-chassis (6.2 s), prusa-mk3s-fan-shroud (1.8 s), ender3-extruder-housing (0.63 s) and thor-art56-gear-plate (0.16 s). The intersection phase (`BOPAlgo_PaveFiller`) takes 10 to 90% depending on the model; building the result takes the rest. Two model-specific hotspots stand out: the gear plate spends 52% in `BOPTools_AlgoTools::IsInvertedSolid` (H7), and the keyboard chassis 21% in 2D point classification (H4) and 11% in `PerformFF` outside the face/face intersections, mostly a nested edge loop (H3).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 9.9% (5.5–14.2) | 10.9% | Sites: `IntSurf_LineOn2S` sequences (`ClearSeq`, `Append`), temporary maps in `MakeBlocks` (`GetFullShapeMap`, `NCollection_BaseMap::Destroy`) | Allocation churn: 64% through `Standard::Allocate`. Per-pair temporary maps (H11) and a faster allocator (H8). |
| 2 | `BSplCLib::Bohm` (TKMath) | 5.3% (0.4–18.0) | 5.3% | `BSplSLib::BuildCache` ← grid evaluation ← `IntCurveSurface_ThePolyhedronOfHInter::Init` ← solid classifier | Span caches built while sampling every face into a polyhedron for the 3D classifier; on the gear plate 18%. Fewer polyhedra with H7. |
| 3 | `GeomAdaptor_Surface::EvalD0` (TKG3d) | 4.4% (1.2–9.9) | 16.7% | `IntCurveSurface_PolyhedronUtils::ComputeMaxDeflection` ← polyhedron init; `math_PSO` in edge/face extrema | Same polyhedra (H7) and extrema sampling. |
| 4 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 3.5% (0.2–5.5) | 3.5% | `BSplSLib_Cache::D0` ← surface evaluation | H6. |
| 5 | `eval_poly0<>` (TKMath) | 2.8% (0.0–7.7) | 2.8% | `BSplSLib_Cache::D0Local` ← polyhedron deflection | Polynomial evaluation inside the polyhedron sampling; numeric (H7 reduces the count). |
| 6 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 2.7% (0.6–7.7) | 2.8% | `BOPAlgo_PaveFiller::PerformFF` directly (keyboard 7.7%) | For every face pair with a curved face, `PerformFF` walks all edge pairs of the two faces and calls `IsClosedFF` for both edges inside the inner loop (H3). |
| 7 | `CSLib_Class2d::internalSiDans` (TKMath) | 2.5% (0.0–11.9) | 2.5% | `CSLib_Class2d::SiDans` ← `IntTools_FClass2d::Perform` ← `IntTools_Context::IsValidPointForFace(s)` ← `MakeBlocks` | Point-in-face test: one scan of the polygon with ON detection plus four more scans for the tolerance corners (H4). |
| 8 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 2.1% (1.3–3.3) | 4.3% | `math_PSO` ← `Extrema_GenExtCS::GlobMinCQuadric` ← `IntTools_EdgeFace` | Particle swarm; algorithm stays. |
| 9 | `CSLib_Class2d::internalSiDansOuOn` (TKMath) | 1.8% (0.0–8.7) | 1.8% | As row 7 | H4. |
| 10 | `math_PSOParticlesPool::GetWorstParticle` (TKMath) | 1.3% (0.0–5.7) | 1.3% | `Extrema_GenExtCS::GlobMinConicS` ← `IntTools_BeanFaceIntersector` | Linear search for the worst particle at every swarm step (5.7% in the fan shroud). Small; a heap would have to keep the tie order. |

### bool-cut

Four models: precious-plastic-shredder-7mm-mesh (18.3 s per run), freecad-plastic-bottle (6.0 s), prusa-mk3s-fan-shroud (1.8 s) and ender3-extruder-housing (0.56 s). Edge/face intersection (`PerformEF`) takes 35% on average and 50 to 58% in the shredder plate and fan shroud. In the shredder plate, a perforated B-spline sheet, every edge/face intersection samples the whole sheet again (H12).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 7.8% (5.1–13.2) | 8.6% | Sites: `IntSurf_LineOn2S` point sequences 19%, `IntTools_FClass2d::Init` 10% | Allocation churn (H8, H14). |
| 2 | `GeomAdaptor_Surface::EvalD0` (TKG3d) | 6.1% (2.9–10.3) | 23.2% | `Extrema_GlobOptFuncConicS` ← `math_PSO` ← `IntTools_BeanFaceIntersector`; `Adaptor3d_TopolTool::BSplSamplePnts` ← `IntCurveSurface_HInter` | Particle swarm for conic edges against surfaces, and sampling of the whole face for each edge (H12). |
| 3 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 4.2% (2.2–5.7) | 4.2% | `BSplSLib_Cache::D0` | H6. |
| 4 | `eval_poly0<>` (TKMath) | 3.7% (0.0–11.8) | 3.7% | `BSplSLib_Cache::D0Local` ← `Adaptor3d_TopolTool::BSplSamplePnts` ← `IntCurveSurface_InterImpl::InternalPerformPolygonBounds` | `IntCurveSurface_HInter` builds a new `Adaptor3d_TopolTool`, samples the B-spline face and builds a polyhedron for every edge it intersects with that face (H12). |
| 5 | `BSplCLib::Bohm` (TKMath) | 2.9% (1.9–4.1) | 2.9% | `BSplSLib::BuildCache` ← grid evaluation ← polyhedron init | Same polyhedra (H12, H7). |
| 6 | `CSLib_Class2d::internalSiDans` (TKMath) | 2.3% (0.1–8.8) | 2.3% | `IntTools_FClass2d::Perform` ← `IsValidPointForFace` ← `IntTools_EdgeFace`, `IsValidBlockForFaces` ← `MakeBlocks` | H4. |
| 7 | `eval_poly0<>` (TKMath) | 2.0% (0.0–7.6) | 2.1% | `BSplSLib_Cache::D0Local` ← `Extrema_GlobOptFuncConicS` | Particle swarm (row 2). |
| 8 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 2.0% (0.7–3.0) | 4.2% | `math_PSO` ← `Extrema_GenExtCS::GlobMinCQuadric` | Particle swarm. |
| 9 | `CSLib_Class2d::internalSiDansOuOn` (TKMath) | 1.8% (0.1–6.8) | 1.8% | As row 6 | H4. |
| 10 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 1.7% (0.5–3.3) | 1.8% | `BOPAlgo_PaveFiller::PerformFF` | H3. |

### bool-common

Four models: launch-keyboard-aluminum-chassis (5.8 s per run), prusa-cw1-rear-cover (4.9 s), zenodo-aerospace-sheet-metal-val18 (0.88 s) and nist-stc-10-ap242 (0.66 s). The rear cover spends 59% in `RefineShell` (H2), and nist-stc-10 70% classifying internal shapes against the solids with a ray whose candidate edges each get a global curve/curve extremum (`math_GlobOptMin` 60%).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 8.6% (6.4–14.0) | 9.4% | Sites: `TopExp::MapShapesAndAncestors`, `IntSurf_LineOn2S` sequences, `NCollection_BaseMap::Destroy` | Allocation churn (H8, H11). |
| 2 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 7.9% (0.4–22.8) | 12.0% | `TopExp_Explorer::Next` ← `FindShape` ← `RefineShell` ← `BOPAlgo_ShellSplitter::SplitBlock` | `RefineShell` finds each shell edge inside its two faces by scanning the faces' edges, then scans them again for internal edges: quadratic in the edges of large faces (H2). |
| 3 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 4.0% (0.5–11.3) | 9.5% | `math_GlobOptMin` ← `Extrema_ExtCC` ← `BRepClass3d_BndBoxTreeSelectorLine::Accept` ← `BOPAlgo_Builder::FillInternalShapes` | Ray/edge tests use a global curve/curve extremum per B-spline edge; numeric, model-specific (nist-stc-10). |
| 4 | `CSLib_Class2d::internalSiDans` (TKMath) | 3.5% (0.1–13.1) | 3.5% | `IntTools_FClass2d::Perform` ← `IsValidPointForFaces` ← `MakeBlocks` | H4. |
| 5 | `BSplCLib::Bohm` (TKMath) | 2.8% (0.2–9.6) | 2.8% | `BSplCLib_Cache::BuildCache` ← `GeomAdaptor_Curve::RebuildCache` ← `EvalD0` | The span cache is rebuilt whenever the global optimizer jumps to another span (cache thrashing); see row 3. |
| 6 | `CSLib_Class2d::internalSiDansOuOn` (TKMath) | 2.5% (0.1–9.7) | 2.5% | As row 4 | H4. |
| 7 | `TopLoc_Location::Multiplied` (TKMath) | 2.5% (0.0–9.8) | 2.8% | `TopoDS_Iterator::updateCurrentShape` ← `FindShape` ← `RefineShell` | Location composition during the repeated scans (H2). |
| 8 | `IntPatch_Polygo::Segment` (TKGeomAlgo) | 2.4% (0.0–9.5) | 2.8% | `Intf_InterferencePolygon2d::Interference` ← `IntPatch_RstInt::PutVertexOnLine` | All-pairs segment interference between 2D polygons when placing vertices on intersection lines; model-specific (sheet metal). Spatial acceleration possible but must keep the pair order. |
| 9 | `~TopLoc_SListOfItemLocation` (TKBRep) | 2.0% (0.2–6.1) | 2.0% | `TopExp::MapShapesAndAncestors`, `FindShape` | Row 7. |
| 10 | `TopLoc_SListOfItemLocation::Assign` (TKMath) | 1.8% (0.1–6.7) | 1.8% | `TopoDS_Iterator::updateCurrentShape` ← `RefineShell` | Row 7. |

### fillet

Five models: precious-plastic-shredder-7mm-mesh (2.7 s per run), nist-ftc-09-ap242 (0.34 s), jubilee-wedge-plate-with-drafts (0.24 s), thor-art56-gear-plate (0.14 s) and ender3-extruder-housing (0.11 s). `ChFi3d::IsTangentFaces` takes 31% on average and 51% and 77% in two models, nearly all of it in building surface adaptors (H1). The shredder plate spends 61% in `BRepTopAdaptor_TopolTool::Classify` on its perforated face (building and querying its 2D classifier), and the wedge plate 82% in corners (`PerformFilletOnVertex`).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 18.2% (5.9–32.3) | 18.9% | Sites: `BRepTopAdaptor_FClass2d` constructor 15%, `Extrema_GGExtPC` 8%, `BRep_Tool::CurveOnPlane` and `GeomProjLib::ProjectOnPlane` 10% | 78% through `Standard::Allocate`. Adaptors, projected pcurves and 2D classifiers rebuilt per call (H1); allocator (H8). |
| 2 | `BSplCLib::Bohm` (TKMath) | 4.3% (0.0–11.2) | 4.3% | `BSplCLib_Cache::BuildCache` ← evaluation during fillet surface computation | Span caches rebuilt as evaluation moves between spans; numeric. |
| 3 | `FindSITargetTypeInstance` (vcruntime140) | 4.3% (1.0–7.5) | 4.3% | `__RTDynamicCast` ← `occ::down_cast` ← `BRepAdaptor_Surface` construction, `BRep_Tool::CurveOnPlane` | H5. |
| 4 | `GeomAdaptor_Surface::EvalD0` (TKG3d) | 2.9% (0.0–10.3) | 9.4% | `Adaptor3d_CurveOnSurface::EvalD0` ← fillet computation | Evaluation inside the fillet walking; numeric. |
| 5 | `eval_poly0<>` (TKMath) | 2.8% (0.0–12.7) | 2.9% | `BSplSLib_Cache::D0Local` | Numeric. |
| 6 | `Geom2dAdaptor_Curve::EvalD0` (TKG2d) | 2.6% (0.1–9.2) | 8.9% | `Geom2dInt` curve/curve intersection in fillet trimming | Numeric. |
| 7 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 2.4% (0.0–6.8) | 2.4% | `BSplCLib_Cache::D0` ← `Geom2dAdaptor_Curve::EvalD0` | H6. |
| 8 | `CSLib_Class2d::internalSiDans` (TKMath) | 2.1% (0.0–9.5) | 2.1% | `BRepTopAdaptor_FClass2d::Perform` ← `ChFi3d_Builder::ComputeData` | H4. |
| 9 | `CSLib_Class2d::internalSiDansOuOn` (TKMath) | 1.7% (0.0–7.5) | 1.7% | As row 8 | H4. |
| 10 | `BSplCLib::Eval` (TKMath) | 1.6% (0.0–5.7) | 2.1% | `Geom2d_BSplineCurve::LocalD0` | Numeric. |

### shell

Five models: ender3-extruder-housing (3.1 s per run), zenodo-tendon-hand-testbench (2.7 s), prusa-cw1-rear-cover (1.5 s), zenodo-aerospace-sheet-metal-val18 (0.91 s) and nist-stc-10-ap242 (0.36 s). 82% of the time is in `BRepOffset_Tool::Inter3D`, which runs a complete `BOPAlgo_PaveFiller` on each pair of adjacent offset faces. Each run creates a new `IntTools_Context`, so the 2D classifier of a face is rebuilt for every pair it belongs to (`IntTools_FClass2d::Init` 12% on average, 33% in zenodo-tendon), and setting up the data structure takes 15%.

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 15.6% (6.9–25.4) | 16.4% | Sites: `IntTools_FClass2d::Init` 14%, sequences, maps | 74% through `Standard::Allocate`. Per-pair contexts rebuild classifiers (H14); allocator (H8). |
| 2 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 2.9% (0.0–6.0) | 2.9% | `BSplCLib_Cache::D0` ← `math_PSO` extrema | H6. |
| 3 | `BSplCLib::Bohm` (TKMath) | 2.9% (0.0–6.4) | 2.9% | `BSplCLib_Cache::BuildCache` ← `IntTools_BeanFaceIntersector` | Span caches; numeric. |
| 4 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 2.5% (0.6–5.6) | 6.5% | `math_PSO` ← `Extrema_GenExtCS` | Particle swarm. |
| 5 | `GeomAdaptor_Surface::EvalD0` (TKG3d) | 2.3% (0.3–4.4) | 6.3% | `math_PSO` ← `Extrema_GenExtCS` | Particle swarm. |
| 6 | `Geom2dAdaptor_Curve::EvalD0` (TKG2d) | 2.1% (0.0–4.1) | 5.0% | `IntTools_FClass2d::Init` | Polygon sampling of each wire's pcurves when a classifier is built (H14). |
| 7 | `GeomInt_BSpParLeastSquare…::MakeTAA` (TKGeomAlgo) | 1.8% (0.0–4.4) | 2.1% | `GeomInt_WLApprox` ← `IntTools_FaceFace` | Numeric. |
| 8 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 1.8% (0.4–3.6) | 2.0% | `BOPDS_DS::InitShape` ← `BOPDS_DS::Init` ← `Inter3D` | Data structure built from scratch for each face pair. |
| 9 | `BOPTools_PairSelector<>::Accept` (TKBO) | 1.5% (0.1–5.0) | 1.9% | `BVH_PairTraverse::Select` ← `BOPDS_Iterator::Intersect` ← `BOPAlgo_PaveFiller::Init` | Bounding-volume tree built and traversed per face pair. |
| 10 | `Adaptor3d_CurveOnSurface::EvalD0` (TKG3d) | 1.4% (0.1–3.8) | 5.7% | `IntTools_FClass2d::Init` | Row 6. |

### mesh-coarse

Five models: framework-16-graphics-module-assembly (40.5 s per run), voron-legacy-assembly (10.2 s), launch-keyboard-v1-assembly (4.5 s), prusa-cw1-rear-cover (0.31 s) and nist-ctc-02-ap203 (0.15 s). The Delaunay circle grid (`NCollection_CellFilter` in `BRepMesh_CircleTool`) takes 19 to 76% (H9), and the allocators that `BRepMesh_Delaun` creates for each face about 11%, up to 15% (H10).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 18.3% (4.2–23.9) | 19.4% | Sites: `NCollection_IncAllocator::clean` 36% and `allocateSlow` 23%, in the allocators of `BRepMesh_Delaun`; frontier checks; map resizing | `BRepMesh_Delaun` creates several `NCollection_IncAllocator`s with 1 MB first blocks for every face. Windows serves blocks that large with `VirtualAlloc`, so every face commits, faults in and releases fresh pages (H10). |
| 2 | `NCollection_CellFilter<>::inspect` (TKMesh) | 16.8% (2.6–47.0) | 17.1% | `BRepMesh_CircleTool::Select` ← `BRepMesh_Delaun::createTrianglesOnNewVertices` | The grid is coarse: cells per direction grow as `2^log10(n)`, about n^0.3, of the vertex count (`BRepMesh_GeomTool::CellsCount`, `AdjustCellsCounts`). On large faces each cell holds hundreds of circumcircles, all tested for every new node (H9). |
| 3 | `memcpy_avx_ermsb_amd` (vcruntime140) | 6.8% (4.4–10.4) | 6.8% | `NCollection_Map::emplaceImpl` ← `NCollection_CellFilter::iterateAdd` ← `BRepMesh_CircleTool::bind` | Each circle is added to every cell its box covers, and each insertion builds a temporary cell key through a `memcpy` call (H9). |
| 4 | `opencascade::MurmurHash::MurmurHash64A` (TKMesh) | 5.9% (3.1–12.3) | 5.9% | As row 3 | A general byte hash for a two-integer cell index, once per covered cell (H9). |
| 5 | `[kernel]` | 2.7% (1.5–4.1) | 13.2% | Page faults in `NCollection_CellFilter::iterateAdd` | First touch of the freshly committed allocator blocks (H10). |
| 6 | `NCollection_Map<>::emplaceImpl<>` (TKMesh) | 2.0% (1.1–4.1) | 15.1% | As row 3 | H9. |
| 7 | `BRepMesh_DataStructureOfDelaun::IndexOf` (TKMesh) | 1.9% (1.0–3.1) | 1.9% | `AddLink` ← `BRepMesh_Delaun::createTriangles` | Hash lookup of links; core of the algorithm. |
| 8 | `BRepMesh_Delaun::createTriangles` (TKMesh) | 1.4% (0.5–2.4) | 25.4% | `createTrianglesOnNewVertices` | Core of the algorithm. |
| 9 | `BRepMesh_DataStructureOfDelaun::cleanLink` (TKMesh) | 1.3% (0.6–2.5) | 1.8% | `RemoveLink` ← `deleteTriangle` | See mesh-fine row 5. |
| 10 | `NCollection_BaseMap::Destroy` (TKernel) | 1.2% (0.3–1.8) | 2.2% | `BRepMesh_Delaun::createTriangles` | Temporary maps per insertion. |

### mesh-fine

Five models: voron-legacy-assembly (38.9 s per run), jubilee-frame-assembly (11.8 s), adafruit-fruit-jam-case (6.4 s), prusa-cw1-rear-cover (1.3 s) and nist-ctc-02-ap203 (0.45 s). The same pattern as coarse meshing: the circle grid takes about 39% (lookups 19%, adding circles to cells about 16%), and the per-face allocators about 5%.

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `NCollection_CellFilter<>::inspect` (TKMesh) | 19.4% (6.7–31.2) | 19.9% | `BRepMesh_CircleTool::Select` ← `BRepMesh_Delaun::createTrianglesOnNewVertices` | Coarse circle grid, long lists per cell (H9). |
| 2 | `[heap]` | 9.7% (8.2–11.7) | 10.3% | Sites: `NCollection_IncAllocator::clean` 33% and `allocateSlow` 20%, map resizing in `BRepMesh_Delaun` and `BRepLib::UpdateDeflection` | Per-face allocators (H10); allocator (H8). |
| 3 | `opencascade::MurmurHash::MurmurHash64A` (TKMesh) | 8.2% (3.2–14.0) | 8.2% | `NCollection_Map::emplaceImpl` ← `NCollection_CellFilter::iterateAdd` ← `BRepMesh_CircleTool::bind` | Byte hash of the cell index, once per covered cell (H9). |
| 4 | `memcpy_avx_ermsb_amd` (vcruntime140) | 6.2% (4.5–8.3) | 6.2% | As row 3 | A temporary cell key is built, with a `memcpy` call, for every covered cell (H9). |
| 5 | `BRepMesh_DataStructureOfDelaun::cleanLink` (TKMesh) | 3.2% (1.3–8.7) | 4.1% | `RemoveLink` ← `BRepMesh_Delaun::deleteTriangle` | Removing a link scans the node's whole link list; nodes with very many links (likely those of the initial bounding triangle) make this quadratic. Changing the storage would change a public class layout; not ranked. |
| 6 | `[kernel]` | 3.2% (2.0–4.9) | 8.3% | Page faults in `NCollection_CellFilter::iterateAdd` | First touch of fresh allocator blocks (H10). |
| 7 | `BRepMesh_DataStructureOfDelaun::IndexOf` (TKMesh) | 2.6% (1.8–3.9) | 2.7% | `AddLink` ← `createTriangles` | Link hash lookup; core of the algorithm. |
| 8 | `CSLib_Class2d::internalSiDans` (TKMath) | 2.1% (0.1–5.0) | 2.1% | `BRepMesh_Classifier::Perform` ← node insertion | Each candidate surface node is classified against the face boundary (H4). |
| 9 | `CSLib_Class2d::internalSiDansOuOn` (TKMath) | 1.9% (0.1–3.6) | 1.9% | As row 8 | H4. |
| 10 | `NCollection_Map<>::emplaceImpl<>` (TKMesh) | 1.8% (1.4–2.4) | 15.7% | `NCollection_CellFilter::iterateAdd` | H9. |

### check

Five models: launch-keyboard-v1-assembly (51.2 s per run), jubilee-frame-assembly (14.4 s), so-arm101-assembly (6.7 s), prusa-cw1-rear-cover (1.0 s) and ender3-hand-twisted-nut (0.29 s). Wire checks take 21 to 41% (`BRepCheck_Wire::InContext`, mostly the self-intersection test of each wire), the classification of a face's wires 28 to 44% (`BRepCheck_Face::ClassifyWires`, which builds a 2D classifier for each wire), and setting up the result objects 4 to 18% (`BRepCheck_Analyzer::Init`). Planar faces again project their edges on every pcurve request (`BRep_Tool::CurveOnPlane` 8 to 25%).

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 20.6% (15.7–24.0) | 22.7% | Sites: `BRepTopAdaptor_FClass2d` constructor 16%, sequences, maps of the result objects, geometry from `CurveOnPlane` | Each wire's classifier samples every pcurve into new point sequences; every sub-shape gets a result object with its own maps, freed at the end (`~BRepCheck_Result`). Allocator (H8), fewer projections (H16). |
| 2 | `Geom2dAdaptor_Curve::EvalD0` (TKG2d) | 4.6% (2.0–6.6) | 15.4% | Polygon construction in `Geom2dInt_GInter::Perform` ← `BRepCheck_Wire::SelfIntersect`, `IntersectWires` | Every pair of edges builds the polygons of both pcurves again, so each pcurve is sampled once per pair it is in (H15). |
| 3 | `BSplCLib::Bohm` (TKMath) | 3.6% (1.6–7.9) | 3.6% | `BSplCLib_Cache::BuildCache` ← `Geom2dAdaptor_Curve::Value` in the polygon sampling | Span caches rebuilt by the repeated sampling (H15). |
| 4 | `opencascade::MurmurHash::MurmurHash64A` (TKTopAlgo) | 3.0% (0.4–5.2) | 3.0% | `std::hash<TopoDS_Shape>` ← `NCollection_IndexedDataMap::lookup` ← `BRepCheck_ParallelAnalyzer` | The analyzer looks up the result of a sub-shape in a shape map at each step; maps take 6 to 18% in all. A cheaper hash changes no result here, but keep map orders in mind. Not ranked. |
| 5 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 3.0% (1.7–4.4) | 4.5% | `TopExp_Explorer::Next` ← `BRepCheck_Vertex::InContext`, `BRepCheck_Analyzer::ValidSub` | Topology walks to find a vertex in its context shape (3 to 5%). |
| 6 | `FindSITargetTypeInstance` (vcruntime140) | 2.6% (1.7–3.6) | 2.6% | `__RTDynamicCast` ← `BRep_Tool::CurveOnPlane`, `Geom2d_TrimmedCurve` constructor | H5. |
| 7 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 2.2% (0.9–3.8) | 2.2% | `BSplCLib_Cache::D0` ← `Geom2dAdaptor_Curve::EvalD0` | H6. |
| 8 | `BRepTopAdaptor_FClass2d::BRepTopAdaptor_FClass2d` (TKTopAlgo) | 2.0% (1.3–3.0) | 22.4% | `BRepCheck_Face::ClassifyWires` ← `OrientationOfWires` | One classifier per wire is inherent; its construction cost is pcurve sampling, `CurveOnPlane` projections and allocation (H16, H8). |
| 9 | `~TopLoc_SListOfItemLocation` (TKBRep) | 1.7% (1.1–2.0) | 1.8% | `BRepCheck_Vertex::InContext` | Row 5. |
| 10 | `TopLoc_Location::Multiplied` (TKMath) | 1.6% (0.2–2.5) | 1.7% | `TopoDS_Iterator::updateCurrentShape` ← `BRepCheck_Analyzer::IsValid` | Row 5. |

### step-read

Five models: jubilee-frame-assembly (16.5 s per run), so-arm101-assembly (4.9 s), prusa-cw1-rear-cover (1.9 s), nist-ftc-09-ap242 (0.25 s) and freecad-cup (0.24 s). Shape healing after translation takes 26 to 90% (`ShapeFix_Shape`), and in three of the models most of it computes missing pcurves by projecting edges onto their surfaces (`ShapeConstruct_ProjectCurveOnSurface` 37 to 70%). Parsing takes 6 to 16%, and 59% in the PMI-heavy nist-ftc-09.

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 14.5% (8.7–23.1) | 15.4% | Sites: `TopoDS_Builder::Add`, `ShapeBuild_ReShape` rebuilding shapes (`BRep_TEdge::EmptyCopy`), strings in the parser, sequences | Healing rebuilds the topology through `ShapeBuild_ReShape` with fresh copies; allocator (H8). |
| 2 | `BSplCLib::Bohm` (TKMath) | 8.8% (0.0–27.8) | 8.8% | `BSplSLib::BuildCache` ← `GeomAdaptor_Surface::RebuildCache` ← `EvalD0` ← `Extrema_GenExtPS` | Point projection evaluates the surface at scattered parameters, so the span cache is rebuilt often (27.8% in the rear cover). Part of H17. |
| 3 | `Extrema_GenExtPS::BuildGrid` (TKGeomBase) | 3.8% (0.0–11.1) | 11.7% | `Extrema_GenExtPS::Perform` ← `ShapeAnalysis_Surface::ValueOfUV` ← `ShapeConstruct_ProjectCurveOnSurface` | For each projected point, the distance to every node, edge and cell of the surface's sample grid is computed, with a surface evaluation per grid edge (H17). |
| 4 | `Extrema_GenExtPS::ComputeEdgeParameters` (TKGeomBase) | 3.3% (0.0–10.3) | 7.2% | `BuildGrid` | As row 3 (H17). |
| 5 | `step::scanner::lex` (TKDESTEP) | 2.9% (0.9–9.0) | 5.0% | `step::parser::parse` ← `StepFile_Read` | Lexer; per-token string allocations. Low priority (file import ranks below modeling). |
| 6 | `BSplCLib_CacheParams::PeriodicNormalization` (TKMath) | 2.5% (0.0–5.8) | 2.5% | `BSplCLib_Cache::D0` ← `Geom2dAdaptor_Curve::EvalD0` | H6. |
| 7 | `step::parser::parse` (TKDESTEP) | 1.4% (0.4–4.7) | 9.0% | `StepFile_Read` | Parser. Low priority. |
| 8 | `FindSITargetTypeInstance` (vcruntime140) | 1.4% (0.6–3.0) | 1.4% | `__RTDynamicCast` ← `BRep_Tool::CurveOnPlane` | H5. |
| 9 | `GeomAdaptor_Curve::EvalD0` (TKG3d) | 1.4% (0.3–4.5) | 4.2% | `BRepLib_ValidateEdge::processApprox` ← `ShapeAnalysis_Edge::CheckSameParameter` | Same-parameter checks sample each edge; numeric. |
| 10 | `[kernel]` | 1.4% (0.5–3.7) | 1.8% | `ReadFile` ← `StepFile_Read` | File reading in small chunks; small. |

### step-write

Five models: thelio-mira-magnet-assembly (11.9 s per run), so-arm101-assembly (3.5 s), launch-keyboard-aluminum-chassis (0.81 s), prusa-cw1-rear-cover (0.42 s) and nist-ftc-09-ap242 (0.10 s). Writing the text takes 17 to 42% (`StepData_StepWriter::SendModel`), translating shapes to STEP entities up to 59%, and the healing step run before writing (`ShapeFix_SplitCommonVertex`) 3 to 48%.

| # | Function | Self | Incl. | Call path | Hypothesis |
| --- | --- | --- | --- | --- | --- |
| 1 | `[heap]` | 23.6% (14.9–33.4) | 26.6% | Sites: `TCollection_AsciiString::reallocate` ← `AssignCat` ← `StepData_StepWriter::Send`; string destructors; `Interface_EntityIterator::AddItem` | Each output line is built by appending small pieces to a string that is reallocated at every append (H19); strings and entity lists are allocated per entity (H8). |
| 2 | `[kernel]` | 3.7% (2.0–4.9) | 4.5% | `WriteFile` ← `StepSelect_WorkLibrary::WriteFile` | File writing; small. |
| 3 | `TopoDS_Iterator::updateCurrentShape` (TKBRep) | 3.6% (1.3–8.3) | 4.1% | `TopExp_Explorer::Next` ← `ShapeFix_SplitCommonVertex::Perform` | For every pair of wires of a face, every vertex of one wire is compared with every vertex of the other by walking the second wire again (H18). |
| 4 | `FindSITargetTypeInstance` (vcruntime140) | 3.3% (2.2–4.0) | 3.3% | `__RTDynamicCast` ← `Handle::DownCast` ← `StepData_StepWriter::Send` | The writer finds each entity's type by down casts (H5). |
| 5 | `__crt_stdio_output::output_processor<>::process` (ucrtbase) | 3.2% (2.1–4.2) | 9.3% | `sprintf` ← `StepData_StepWriter::Send` | Numbers are formatted with `sprintf`; the format must stay byte-identical, so only cheaper equivalent formatting would do. Low priority. |
| 6 | `Interface_InterfaceModel::Number` (TKXSBase) | 2.6% (1.1–5.1) | 2.6% | `Interface_Graph::Evaluate` ← `IFSelect_ModelCopier::SendAll` | Entity numbering through a map lookup per reference. |
| 7 | `ShapeFix_SplitCommonVertex::Perform` (TKShHealing) | 2.4% (0.2–6.4) | 16.7% | `splitcommonvertex` ← `ShapeProcess::Perform` | H18 (47.7% of the time in the Thelio assembly). |
| 8 | `~TopLoc_SListOfItemLocation` (TKBRep) | 2.3% (0.4–7.6) | 2.3% | `ShapeFix_SplitCommonVertex::Perform` | H18. |
| 9 | `NCollection_LocalArray<>::Reallocate` (TKBRep) | 1.5% (0.2–5.0) | 1.5% | `TopExp_Explorer::pushIterator` ← `ShapeFix_SplitCommonVertex::Perform` | Explorer stacks reallocated for each new explorer (H18). |
| 10 | `strlen` (ucrtbase) | 1.5% (0.7–2.1) | 1.5% | `Interface_FloatWriter::Write` ← `StepData_StepWriter::Send` | Float formatting; as row 5. |

### All cores

Thirteen jobs were also profiled with `--threads all`. Speedups are from the baseline results. "Working threads" counts threads doing OCCT work, on average over the operation; "One worker" is the share of the operation's time when at most one thread did OCCT work; "Scheduler" is the share of all CPU samples in TBB workers waiting for work (spinning, or yielding with `SwitchToThread`).

| Job | Model | Speedup | Working threads | One worker | Scheduler | What limits it |
| --- | --- | --- | --- | --- | --- | --- |
| rebuild | heatsink | 0.99× | 1.0 | 98% | 11% | The fillet code is serial; `IsTangentFaces` alone is 71% of the time (H1). |
| rebuild | housing | 2.00× | 3.7 | 35% | 44% | Short parallel loops in the booleans; idle workers spin between them (H20). |
| bool-fuse | freecad-plastic-bottle | 4.25× | 6.2 | 43% | 4% | Serial setup and `MakeBlocks`, the solid classifier (H7). |
| bool-fuse | launch-keyboard-aluminum-chassis | 1.36× | 1.9 | 86% | 6% | `MakeBlocks` is serial: point classification 10% (H4), `PerformFF`'s edge loop 7% (H3), `GetStickVertices` (H11). |
| bool-fuse | prusa-cw1-rear-cover | 1.16× | 1.4 | 94% | 6% | `RefineShell` runs on one thread, 42% of the time (H2). |
| bool-cut | precious-plastic-shredder-7mm-mesh | 2.49× | 5.1 | 56% | 1% | Serial `MakeBlocks` and classification (H4). |
| shell | zenodo-tendon-hand-testbench | 0.70× | 1.9 | 29% | 71% | Thousands of tiny parallel loops, one set per face pair in `BRepOffset_Tool::Inter3D`; idle workers spin and yield between them, so all cores are slower than one (H20). |
| shell | adafruit-fruit-jam-case | 0.67× | 2.6 | 13% | 78% | As above (H20). |
| mesh-coarse | framework-16-graphics-module-assembly | 3.09× | 4.5 | 6% | 1% | Faces run in parallel, but a few large faces dominate; the circle grid is 39% of all CPU time (H9). |
| mesh-fine | voron-legacy-assembly | 3.10× | 4.8 | 14% | 1% | As above; the Windows heap's lock backoff (`RtlBackoff`) shows up (H8, H10). |
| mesh-fine | jubilee-frame-assembly | 4.52× | 8.0 | 27% | 1% | Building the mesh model (`IMeshTools_ModelBuilder`) is serial; heap lock backoff 3% (H8, H10). |
| check | jubilee-frame-assembly | 3.25× | 5.1 | 65% | 0% | Creating and freeing the per-shape result objects is serial (`BRepCheck_Analyzer::Init`, `~BRepCheck_Result`, H8). |
| check | launch-keyboard-v1-assembly | 2.11× | 3.2 | 82% | 0% | As above, about 20% of the time, plus `BRepCheck_Solid::Minimum` building a solid classifier (H7). |

Every operation gives the same topology and agrees within 1e-12 in volume and area between single-threaded and all-cores runs in the baseline, so running small loops serially, or running more in parallel, keeps outputs within the SPEC.md 7.1 rule. No lock showed heavy contention: waits and locks are under 6% of samples, except while meshing (7 to 9%), where the Windows heap's lock backoff (`RtlBackoff`) reaches 3%.

## Entries

### 2026-10-15: fillet: planar shortcut in `ChFi3d::IsTangentFaces` (H1)

- **Hotspot:** `ChFi3d::IsTangentFaces` (TKFillet) takes 31% of fillet time (mean of the five profiled models, 77% in nist-ftc-09), 17% of rebuild (83% in the heatsink part) and 7% of shell. The fillet builder calls it while chaining edges (`ChFi3d_Builder::FaceTangency`, `PerformExtremity`) and counting sharp edges at vertices (`ChFi3d_NumberOfSharpEdges`); shell calls it for every edge in `BRepOffset_Analyse::Perform`. 85 to 98% of its time goes into building a `BRepAdaptor_Surface` and a `BRepTopAdaptor_TopolTool` for each of the two faces.
- **Expected change and gain** (written before coding): the adaptors and tools are built only to read `NbSamples()`, the number of samples along the edge. For a plane it is always 10 × 10 = 100 (`BRepTopAdaptor_TopolTool::ComputeSamplePoints`), yet building them projects every edge of the face onto the plane twice: once in `BRepTools::UVBounds` and once for each edge's `BRepAdaptor_Curve2d`. A file-local helper returns 100 for a face whose surface type is `GeomAbs_Plane` without building either, and other faces go through the same code as before. The sample loop therefore runs with the same count, and results stay the same. The projections alone (`CurveOnPlane` under the constructors) take 66% of the heatsink rebuild, 67% of the nist-ftc-09 fillet and 42% of the ender3 housing fillet. Expected gains:
  - heatsink rebuild: −65 to −80%
  - rebuild: −12 to −18% (geometric mean)
  - fillet: −25 to −35% (geometric mean over the profiled models)
  - shell: −5 to −7%
  - other operations: unchanged, because they do not call the function

  Behavior can differ only on invalid input. If an edge of a planar face cannot be projected or has a reversed parameter range, building the tool raised an exception, and the helper does not.
- **Change:** in `src/ModelingAlgorithms/TKFillet/ChFi3d/ChFi3d.cxx`, the new file-local function `nbSamplesOfFace` returns 100 when `BRepAdaptor_Surface(face, false).GetType()` is `GeomAbs_Plane`. That adaptor is built without the face's bounds, and the type depends only on the surface. For other faces, the function builds the adaptor and the tool as before. `IsTangentFaces` calls it for both faces. Only `TKFillet.dll` changes.
- **Before -> after:** geometric means, single-threaded and all cores.
  - Full run on 2026-10-16 (`bench/results/2026-10-16-h1.json`, 612 jobs) against `baseline-8.0.1.json`:
    - fillet: −35.1% and −35.2% over 28 models (kicad heatsink −81%, nist-ftc-09 −76%, ender3 assembly −74%)
    - rebuild: −28.2% and −29.2% (heatsink −82%, bottle −38%, bracket −29%; the other five parts within noise)
    - shell: −5.3% and −4.9% over 18 models (thelio-mega −36%, launch keyboard and jubilee frame −13%)
    - step-read: +3.3% and +3.4%; check: +2.5% and +2.2%; the other job types −1.6 to +2.0%. The machine itself became slower after a Windows update on 2026-10-14, after the baseline was measured (decisions.md, 2026-10-16), so these differences do not come from this change.
  - Unmodified 8.0.1, then this change, timed back to back on the same night (`2026-10-16-base-subset.json` and `2026-10-16-h1-subset2.json`; 12 models and the 8 rebuild parts, 240 jobs):
    - fillet: −51.3% and −50.5% over 11 models
    - rebuild: −26.6% and −29.2%
    - shell: −3.4% and −3.4% over 9 models
    - every other job type: −0.7 to +1.2%
- **Verification:**
  - Fingerprints: all 612 jobs of the full run and all 240 of the same-night runs are within 1e-9 of their reference, with identical topology counts and validity.
  - Test grid: no test that passed in the baseline fails, in one run with this build and two with the check build below. Two tests that upstream marks as failing at random (`TODO ?`) passed in some runs. `offset compshape A3` gave the correct result 12 times and the faulty one 8 times out of 20, both with unmodified 8.0.1 and with this change. `pipe bugs bug24909_2` does not reach the fillet code.
  - Equivalence: a temporary build also ran the old construction for every planar face. It checked at least 149,515 calls across the test grid and the rebuild, fillet and shell jobs. The tool returned 100 every time and never raised an exception.
  - Speed: fillet is 35% faster in the full run and 51% faster on the same-night subset, against the 3% required. Against unmodified 8.0.1 timed the same night, no other operation is slower by more than 2% (largest: bool-cut single-threaded, +1.2%). Against the baseline, step-read and check are 2.2 to 3.4% slower, but on the 12 same-night models, unmodified 8.0.1 differs from the baseline as much as this change does (step-read +1.8% and +2.1%, against +1.9% and +2.0%). Only the fillet builder and `BRepOffset_Analyse` (shell) call `IsTangentFaces`.
- **Result:** accepted. Fillet (−35%) is at the top of the expected range and rebuild (−28% and −29%) beats it; shell gained less than expected on the same night (−3.4% against −5 to −7%). `2026-10-16-h1.json` is the latest accepted results for the next cycle.
