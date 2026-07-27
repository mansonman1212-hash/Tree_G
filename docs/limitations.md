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

## 3a. Measured behaviour, and the one gap that dominates everything

Numbers below are measured on the current build: standard quality, open-grown
environment, default seed.

### The dominant limitation: the skeleton is far too sparse in fine twigs

This has now been measured **four independent ways**, and it is the single issue
that limits realism more than anything else:

| Measurement | Broadleaf, 80 yr | What a real tree shows |
|---|---|---|
| Living terminal shoots | 557 | order of 10⁵ |
| Total leaf area | 8 m² | several hundred m² |
| Attractors consumed | 18% of the cloud | most of it |
| Crown radius vs its own envelope | 7.7 m of 9.6 m | fills it |

The four are the same fact seen from different angles. Foliage is borne on living
terminal shoots, so 557 tips can only carry a few square metres of leaf however
generously each shoot is counted.

The consequence propagates: the pipe model calibrates branch radii against
supported foliage area, so 100× too little foliage makes branches roughly 6×
too thin (100^(1/2.49)), and branches that thin cannot carry the wood mass hanging
from them. That is why the deflection pass reports **216 of 16 952 segments
hitting the rotation clamp** on a mature tree, and why a 250-year tree deflects
21 m. The mechanics is behaving correctly for the input it is given; the input is
wrong.

Raising `max_branch_order` from 5 to 8 (and enlarging the per-order arrays from 6
to 10 slots, which was the binding constraint) improved the 250-year case from 259
to 1 786 terminal shoots but barely moved the 80-year case. So the array size was
*a* limit, not *the* limit — the remaining constraint is the rate at which the
growth simulation creates and keeps higher-order shoots, which is governed by the
bud-break probability, the crown envelope stop, and shade mortality. Those three
are directly coupled: raising branching improves density and worsens mortality.

`tree_mechanics_run` now emits a runtime warning whenever more than 1% of segments
hit the rotation clamp, naming the likely cause, so this surfaces on every run
instead of being hidden. The test suite prints it too.

### Other measurements

| Quantity | Broadleaf 80 yr | Conifer 80 yr |
|---|---|---|
| Shoot segments | ~14 000 | ~27 000 |
| Realised height | 16.7 m (target 18.8 m) | 27.5 m (target 27.3 m) |
| Trunk base radius | 0.260 m (0.168 m before flare, ×1.55) | 0.341 m (×1.40) |
| Wood mass | 1 957 kg | 2 444 kg |
| Root radius / crown radius | 2.16 | 2.59 |
| Dead shoot segments | 33.1% | 78.4% |
| Own-bend sign violations | 0 | 0 |

**KNOWN DEFECT: conifer self-shading mortality.** 78.4% of conifer shoot segments
die, against a believed-correct figure under 40%. The cause is measured: the
conifer carries about nine times the broadleaf's shoot density per unit crown
volume, so crowding drives it, not the mortality threshold. A regression gate in
`test_tree_growth.c` locks the value just above the measurement so it cannot
worsen silently, and is labelled in the source as recording a defect rather than
endorsing one.

### Why tuning stopped here

Crown density, self-shading mortality and branch radii form one coupled system.
Every remaining lever trades one against another, and the project directive is
explicit that visual quality must be judged visually. Continuing to tune blind
would be optimising numbers with no way to tell whether the tree looks right.
**The headless validation rasteriser is the unblocking step.**

## 4. Tooling gaps in the development sandbox## 4. Tooling gaps in the development sandbox

| Gap | Effect | Mitigation |
|---|---|---|
| AddressSanitizer / UBSan runtimes not installed | `host-build.sh asan` compiles but cannot run | The mode is retained for the user's machine and for CI; memory discipline is meanwhile covered by the arena/array design, the leak gate, and injected allocation-failure tests |
| No MSVC | MSVC-specific warnings (`/W4`) and `/std:c17` conformance are unverified | Code is written to the intersection of C17 as implemented by MSVC, clang, and gcc; two independent compilers already agree with an aggressive warning set |
| No GPU, no display | Nothing about rendering can be observed | A CPU validation rasteriser will be added so generated geometry can be inspected visually as image captures |

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
