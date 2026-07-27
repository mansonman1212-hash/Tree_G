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
- Tree layer: mechanics (radii, sag, reaction wood), bark, leaves, needles,
  damage, build orchestration, construction stage records.
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
(shoot system and root system). A deterministic, validated biological skeleton
is generated for both categories. No geometry is produced yet: radii are unset
and there is no surface.

Verified by 348 test cases / 513 279 checks passing under clang and gcc, in
debug and release, with zero warnings under `-Werror` and an aggressive warning
set, producing byte-identical output across all four configurations. Details and
the specific failure modes each test targets are in `docs/testing.md`.

## 3a. Measured behaviour of the growth simulation, and where it is not yet right

Numbers below are measured on the current build: age 80, standard quality,
open-grown environment, default seed.

| Quantity | Broadleaf | Conifer | Assessment |
|---|---|---|---|
| Shoot segments | 6 917 | 26 679 | plausible |
| Realised height | 16.7 m (target 18.8 m) | 27.5 m (target 27.3 m) | acceptable; broadleaf 11% under |
| Crown radius | 7.72 m (envelope 9.60 m) | 4.73 m (envelope 4.74 m) | broadleaf fills 80% of its envelope |
| Root radius / crown radius | 2.16 | 2.59 | within the 1–3x field range |
| Attractors consumed | 18.2% | 61.2% | broadleaf under-claims its envelope |
| Attractor density realised vs profile | 2.76 vs 2.50 /m³ | 5.55 vs 5.00 /m³ | within 11% |
| **Dead shoot segments** | **33.1%** | **78.4%** | broadleaf plausible; **conifer is wrong** |

**KNOWN DEFECT: conifer self-shading mortality is too high.** 78.4% of conifer
shoot segments die of self-shading, against a believed-correct figure under 40%.
The cause is measured and understood: the conifer's narrow conical envelope
combined with an annual whorl of laterals gives it about nine times the
broadleaf's shoot density per unit crown volume (15.4 vs 1.77 segments/m³), and
the crowding — not the mortality threshold — is the driver. Reducing the branch
order from 4 to 3 and the whorl success rate from 0.90 to 0.62 brought it down
from 88.7%, but no further progress is defensible without being able to LOOK at
the crown. Blind numeric tuning was therefore stopped rather than continued.

A regression gate in `test_tree_growth.c` locks the value just above the measured
78.4% so it cannot silently worsen, and is labelled in the source as recording a
defect rather than endorsing one.

**Related, lower severity:** the broadleaf claims only 18.2% of its attractor
cloud and reaches 80% of its envelope radius. Crown filling density and
self-shading mortality are directly coupled — raising the branch-break
probability improves filling and worsens mortality — so both need to be
calibrated together against visual reference.

**What unblocks these:** the headless validation rasteriser. Until generated
geometry can be inspected as an image, these two quantities cannot be judged, and
the project directive is explicit that visual quality must be assessed visually.
This is the next engineering step.

## 4. Tooling gaps in the development sandbox

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
