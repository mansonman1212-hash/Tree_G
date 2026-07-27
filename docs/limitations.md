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
- Tree layer: profiles, biological graph, growth simulation, roots, mechanics,
  bark, leaves, needles, damage, build orchestration, construction stage records.
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

Verified by 210 test cases / 474 357 checks passing under clang and gcc, in
debug and release, with zero warnings under `-Werror` and an aggressive warning
set, producing byte-identical output across all four configurations. Details and
the specific failure modes each test targets are in `docs/testing.md`.

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
