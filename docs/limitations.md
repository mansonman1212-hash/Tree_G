# Known Limitations and Honest Status

This file states what is and is not true of the project right now. It is updated
with every phase. Anything not listed as implemented and verified should be
assumed absent.

## 1. Development environment mismatch (most important)

The target environment is Windows 11 + MSVC 14.44 + Windows SDK 10.0.26100 +
Direct3D 12. The environment this work is being carried out in is **Linux
x86-64 with clang 15 and gcc 11**.

Consequences, stated plainly:

- The Windows platform layer, the D3D12 renderer, and the HLSL shaders **cannot
  be compiled or executed here**. When they are written, they will be marked as
  *authored but not compiled on the target toolchain* until the user builds them.
- No claim about the running application — window creation, device
  initialisation, frame timing, on-screen appearance, interaction feel — can be
  made from this environment. Such claims will not be made.
- The architecture was designed around this constraint rather than in spite of
  it: Layers 0–2 contain the entire biological and geometric product and are
  fully testable without a GPU.

## 2. Not yet implemented

As of the current commit, the following exist as design decisions in
`docs/research.md` and `docs/architecture.md` and as nothing else:

- Geometry layer: junction meshing, BVH.
- Tree layer: bark, leaves, needles, damage, build orchestration, construction
  stage records.
- Render layer: Win32 platform, D3D12 device, raster PBR renderer, progressive
  renderer, lighting, picking.
- App layer: UI, construction replay controls, inspection panels.
- HLSL shaders.
- `build-and-run.cmd`.
- Headless reference generator and validation-capture rasteriser.

## 3. Implemented and verified

**Layer 0 (core)** — `core_types.h`, `log`, `mem`, `math3d`, `hash`, `rng`.

**Layer 1 (geom), partial** — `mesh`, `mesh_validate`, `spatial`, `camera`.
Still missing from this layer: `mesh_junction` and `bvh`.

**Layer 2 (tree), partial** — `tree_profile`, `tree_graph`, `tree_growth`
(shoot and root systems), `tree_mechanics` (foliage area, pipe-model radii,
supported mass, static deflection, reaction-wood eccentricity, frame rebuild).
A deterministic, validated skeleton with thickness and load response is produced
for both categories. There is still no SURFACE: no cross-sections, no triangles.

Verified by 384 test cases / 532 366 checks passing under clang and gcc, in
debug and release, with zero warnings under `-Werror` and an aggressive warning
set, producing byte-identical output across all four configurations. Details and
the specific failure modes each test targets are in `docs/testing.md`.

## 3a. Measured behaviour

Numbers are measured on the current build, open-grown environment, default seed,
`draft` quality unless stated. `tools/host-refgen.sh` reproduces all of them.

### What the previous version of this section said, and why it is gone

It reported that the skeleton was "far too sparse in fine twigs": 557 living
terminal shoots and 8 m2 of leaf area on an 80-year broadleaf, against real
figures of order 10^5 and several hundred m2. That diagnosis was correct and the
cause has now been found and fixed. Five defects were compounding, each measured
before and after:

| Defect | Evidence it was real | Fix |
|---|---|---|
| One internode per year gave each shoot one chance to branch per year | 423 of 825 axes born in the final tenth of the tree's life; rendered as a bare pole with a tuft | An annual shoot is a multi-node flush (`nodes_per_flush`) |
| Node count held at the profile maximum while the annual increment decayed, so internode LENGTH collapsed instead | Order-4 axes: 0.351 m of length over 41.2 internodes -- **8.5 mm each** | Node count follows vigour, length does not: the long-shoot / short-shoot distinction |
| Every living shoot emitted at least one segment per year however little it grew | Hard organ floor of axes x years = 480 000 organs, unreducible by any detail setting | Extension accumulates in `pending_len` until it is worth a ring |
| The MATURE crown envelope was applied to a sapling | Every lateral below the mature crown base was retired on its first node from year one | Envelope scaled self-similarly to the height reached |
| Reaching the crown surface killed a shoot permanently | A lateral touching the sapling's crown surface was retired for the remaining seventy years | Reaching the surface is a pause (`at_surface`), re-tested every step |

Two further bugs were found in the process and are worth recording because
neither was a tuning question:

- **`tree_graph_kill_subtree` was O(total organs) per call.** It marked the root
  of the subtree and then swept every organ with a higher id, relying on the
  ascending-id invariant. Correct, and quadratic. Measured on a 100-year
  broadleaf: 30 000 shade deaths across a 986 000-organ graph, and generation took
  **83 seconds** against 9 seconds for the 80-year tree. Replaced with an
  explicit-state depth-first walk of the subtree links: the same tree now takes
  **5.7 seconds**, a 14x improvement, and a 220-year individual went from over two
  minutes to under seven seconds.
- **A use-after-realloc in the growth loop.** The live `ShootState *` and
  `const Axis *` were held across `shoot_add` and `tree_graph_add_axis`, either of
  which can reallocate and move the array being pointed into. It segfaulted every
  conifer, whose whorled nodes append several axes inside one internode. Both
  pointers are now refetched after each append.

The light model was also replaced. Per-shoot ray marching cost 49 grid samples for
every live apex on every step -- of order two billion samples on a 220-year tree.
Optical depth is now swept once per step over the density grid, in the manner of
the shadow propagation in Palubicki et al. (2009), making every light query a
single lookup.

### Where the trees stand now

| Quantity | Broadleaf 80 yr | Conifer 80 yr | Broadleaf 12 yr |
|---|---|---|---|
| Growth steps completed | 80 of 80 | 80 of 80 | 12 of 12 |
| Organs / axes | 614 133 / 97 108 | 614 844 / 75 351 | 548 / 129 |
| Shoot segments | 517 056 | 539 521 | 447 |
| Living terminal shoots | 64 361 | 68 915 | 68 |
| Leaf area | 717 m2 | 385 m2 | 1.2 m2 |
| Realised height | 17.93 m (target 18.79) | 26.80 m (target 27.29) | 4.61 m (target 4.60) |
| Trunk base radius | 0.260 m | 0.341 m | 0.064 m |
| Wood mass | 2 481 kg | 2 283 kg | 18 kg |
| Max branch order | 8 | 3 | 5 |
| Self-pruned shoots | 92 618 (15.1% of organs) | 0 | 0 |
| Mesh | VALID, 0 boundary edges | VALID, 0 boundary edges | VALID, 0 boundary edges |
| Generation time | 2.3 s | 2.8 s | <0.01 s |

Terminal shoots and leaf area are in the right order of magnitude for the first
time: 64 361 living tips carrying 717 m2 of leaf, against 557 tips and 8 m2
before. Realised height now matches the resolved target to within 5% because the
leader is driven BY the height curve rather than coincidentally agreeing with it,
and the 12-year tree lands within 0.01 m.

717 m2 over a crown of roughly 19 m width is a leaf area index near 2.5, which is
at the low end of the 4-7 a real closed broadleaf crown shows. So the crown is
now the right order of magnitude and still somewhat too open -- a much better
place to be than two orders of magnitude out, and the remaining factor is a
calibration question rather than a structural one.

### Remaining defects, stated plainly

1. **Very old individuals still hit the organ ceiling.** A 220-year broadleaf
   completes 107 of 220 steps at draft quality and reaches 20.0 m of a 24.99 m
   target. The cause is that the model has no crown retrenchment: a real ancient
   tree sheds its outer crown and its living shoot population reaches a steady
   state, whereas this model accumulates axes indefinitely because shade mortality
   (14%) never overtakes bud break. Raising the extinction coefficient does not
   fix it -- tested at 0.055, 0.10, 0.16 and 0.24, the crown self-regulates through
   reduced bud break instead of increased death, and total organs moved by under
   15%. `hit_organ_limit` is reported on every run and `refgen` prints
   `TRUNCATED` in the growth line.
2. **Conifer self-shading mortality is zero.** Not merely low: no conifer shoot
   dies. 36 469 shoots are pinned at the crown surface and none is ever
   overtopped, so an 80-year conifer keeps every branch it ever grew, including
   its lowest whorls. A real fir sheds its lower crown.
3. **The conifer's upper leader is too bare.** The leader now tracks the height
   curve and outruns its own laterals, leaving roughly the top quarter as an
   almost naked spire.
4. **The broadleaf crown silhouette still reads as its envelope.** The widest
   point sits low and the top comes to a point, where a decurrent broadleaf should
   be widest near mid-crown with a rounded, slightly flattened top.
5. **Mesh size is far beyond a real-time budget.** An 80-year broadleaf skins to
   roughly 11 million triangles and a 220-year one to 20.9 million, at 575 MiB and
   1 156 MiB of CPU geometry. Every branch is a separate closed tube, so most of
   those triangles are inside other triangles. `mesh_junction` (welding unions) and
   a longitudinal mesh LOD are the next work, and until they exist the vertex count
   should be read as an upper bound, not a target.
6. **Mesh validation cannot run on the largest tree.** The 220-year case reports
   `scratch_limit_exceeded` and is then printed as `INVALID`, which conflates
   "failed validation" with "could not be validated". The report is wrong even
   though the diagnosis is honest; the two states must be distinguished.
7. **Quality levels are not purely a level of detail.** `internode_geometry_scale`
   groups botanical nodes into geometric internodes, and because a shoot whose
   annual increment does not complete one geometric internode produces no laterals
   that year, a draft tree branches slightly less than a reference tree rather than
   being the same tree drawn more coarsely. The difference is confined to the
   finest orders, but it is a difference.

### What was checked visually, and what it showed

The CPU rasteriser was the instrument that made all of the above findable. The
first captures after the flush rewrite showed a bare pole under a flat pancake of
twigs -- a result that every numeric check had passed. The current captures show a
dense rounded crown over a clean bole for the broadleaf and a clean cone with a
single leader for the conifer, with the defects listed above visible on
inspection.

## 4. Tooling gaps in the development sandbox

| Gap | Effect | Mitigation |
|---|---|---|
| AddressSanitizer / UBSan runtimes not installed | `host-build.sh asan` compiles but cannot run | The mode is retained for the user's machine and for CI; memory discipline is meanwhile covered by the arena/array design, the leak gate, and injected allocation-failure tests |
| No MSVC | MSVC-specific warnings (`/W4`) and `/std:c17` conformance are unverified | Code is written to the intersection of C17 as implemented by MSVC, clang, and gcc; two independent compilers already agree with an aggressive warning set |
| No GPU, no display | Nothing about rendering can be observed | `tools/swrast.c` rasterises the generated geometry on the CPU using the engine's own camera and winding conventions, and `tools/host-refgen.sh` writes PNG captures that can be inspected directly. This found five structural defects that no numeric test had caught |
| No background processes survive between commands in this sandbox | Long runs must complete inside one command | Generation is now fast enough (2-3 s per mature tree) that this no longer matters |

## 5. Deliberate approximations already recorded

From `docs/research.md`, restated here so they are visible in one place:

- Two architectural classes (Rauh-like, Massart-like), not the full
  Hallé–Oldeman–Tomlinson set of twenty-three.
- Bud-fate signalling is a scalar resource flux, not hormone chemistry.
- Radial growth uses a single per-profile Leonardo exponent plus an
  age-dependent flare term; species-specific tip-to-base conduit widening is not
  reproduced.
- Mechanics are static, solved once. There is no creep, viscoelasticity, or wind
  dynamics — post-generation motion is forbidden by the project directive.
- The reference forms are habits, not species. The broadleaf profile is
  oak-*like*; it is not a botanical identification.

## 6. Determinism, stated precisely

Guaranteed: bitwise-identical organ graph, vertex positions, indices, and
material data for identical `(profile, age, environment, damage, quality, seed)`
on the **same build, same compiler, same optimisation level, same CPU
architecture**.

Not guaranteed, and not claimed: bitwise equality across compilers, across
optimisation levels, or across instruction-set architectures. Floating-point
contraction and libm transcendental implementations differ. Tests therefore
compare fingerprints *within* a run and assert invariants and tolerances rather
than hard-coded magic constants.

## 7. Terms this project will not use loosely

- "Physically accurate" — only where the physics is genuinely solved and the
  approximation is documented.
- "Global illumination" — not applied to ambient darkening or an AO term.
- "Path tracing" — only if a real multi-bounce estimator exists.
- "Production-ready" — only after the §40 acceptance criteria have been evaluated
  item by item, with the unverified items named.
- "Watertight" — only for the woody surface section, and only when
  `mesh_validate` has confirmed it. Peeling bark flakes are intentionally
  non-manifold and live in a separately tagged mesh section.
