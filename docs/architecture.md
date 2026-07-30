# Architecture

## 1. Layering rule

Dependencies point downward only. A lower layer never includes a higher layer's
header. This is enforced by review and by the host build, which compiles layers
0–2 with **no Windows headers available at all** — a link or compile failure is
the enforcement mechanism, not a convention.

```
Layer 4  app          app.c, ui.c, construction_replay.c, picking.c
             |
Layer 3  render       renderer_d3d12.c, renderer_raster.c, renderer_path.c,
             |        lighting.c, platform_win32.c, input.c        [WINDOWS ONLY]
             |
Layer 2  tree         tree_profile, tree_graph, tree_growth, tree_mechanics,
             |        tree_roots, tree_bark, tree_leaf, tree_conifer,
             |        tree_damage, tree_skin, tree_build           [PORTABLE]
             |
Layer 1  geom         mesh, mesh_junction, mesh_validate, spatial, bvh, camera
             |                                                    [PORTABLE]
Layer 0  core         core_types, mem, log, math3d, rng, hash      [PORTABLE]
```

Camera lives in Layer 1, not Layer 3, and is pure math with no device state.
This is what makes the §31.3 static-object regression test possible without a GPU.

## 2. Why the portable/Windows split is load-bearing

Layers 0–2 constitute the entire biological and geometric product. They:

- contain no `#include <windows.h>`, no COM, no HLSL dependency;
- are compiled and tested on the host CI/dev machine by `tools/host-build.sh`;
- are compiled identically on Windows by `build-and-run.cmd`.

Consequence: determinism, mesh validity, watertightness, radius invariants, and
geometry immutability are all verifiable **without a GPU or a window**. The
Windows layer is then responsible for exactly one thing: displaying and probing
an already-validated immutable mesh.

## 3. Ownership and lifetime table

| Resource | Allocated by | Freed by | Mutable during | Immutable after |
|---|---|---|---|---|
| `Arena` blocks | `arena_init` (malloc) | `arena_release` | always | n/a |
| Generation scratch | `TreeBuild.scratch` arena | reset per stage | its stage | n/a |
| `TreeGraph` organ array | `tree_graph_create` (build arena) | `tree_build_destroy` | growth phases | finalisation |
| `Mesh` vertex/index arrays | `mesh_create` (result arena) | `tree_result_destroy` | meshing phases | finalisation |
| `TreeResult` (graph+mesh+stats+hash) | `tree_build_finalize` | `tree_result_destroy` | never | from creation |
| `Bvh` | `bvh_build` (result arena) | with `TreeResult` | build only | after build |
| D3D12 device/queue/swapchain | `renderer_create` | `renderer_destroy` | n/a | n/a |
| GPU vertex/index buffers | `renderer_upload_tree` | deferred-release queue, fence-gated | upload only | after upload |
| Shader blobs | `renderer_create` | immediately after PSO creation | n/a | n/a |
| Previous `TreeResult` on regenerate | — | only after the new one validates | — | — |

**Regeneration rule (directive §27):** a failed generation must not destroy the
last valid tree. `app_generate` builds into a *new* `TreeResult`; the live
pointer is swapped only after `mesh_validate` and `tree_result_hash` succeed.
The old result is released on the next frame boundary after the GPU fence for
its buffers has passed.

## 4. Stable IDs, never long-lived pointers

Organs, vertices, and triangles are addressed by `u32` indices into arrays that
may grow during generation. No structure stores a pointer into a growable array
across a call that can grow it. Accessors are `tree_graph_organ(g, id)`, and the
debug build asserts `id < count` on every access.

## 5. Overflow policy

Every vertex/index/triangle count and every allocation size is computed in
`u64` through `ckd_mul_u64` / `ckd_add_u64`, which return `false` on overflow.
`mesh_reserve` refuses any request whose byte size exceeds a configured hard cap
(default 3 GiB CPU-side) and reports it as a normal, recoverable generation
failure rather than aborting.

## 6. Generation pipeline and stage recording

`tree_build_run` executes a fixed ordered stage list. Each stage:

1. declares which arenas it may write;
2. emits a `ConstructionStage` record containing the *real* counts and, for
   visual stages, the real index range added to the mesh;
3. checks the cancellation flag and may return `TREE_BUILD_CANCELLED` cleanly.

The construction replay system (§20 of the directive) renders prefixes of the
already-built authoritative arrays. It does **not** re-run generation, and it
does not reveal a hidden finished model — the stage records carry explicit
`[first_index, index_count)` spans plus point/edge debug spans, so a replay frame
draws exactly the geometry that existed at that stage and nothing more. Counts
displayed in the UI are read from these records, never estimated.

## 7. Threading model

- Generation runs on **one** worker thread. The UI thread never blocks on it.
- Inside generation, three stages use a deterministic parallel-for over a fixed
  partition (`leaf instantiation`, `bark patch displacement`, `BVH SAH binning`).
  Determinism is preserved because each work item writes only to its own
  pre-reserved output span and draws randomness from a substream keyed by its
  own stable ID, never from a shared stream.
- No stage reads another stage's partially written output.
- The renderer touches `TreeResult` read-only, after finalisation, always.

## 8. Error and failure paths

Single convention: functions that can fail return a `TgResult` enum and write
their output through a pointer. `TG_OK` is zero. Every failure path logs
subsystem, operation, code, and whether the previous tree survives, per §38.

Device loss: `renderer_present` detects `DXGI_ERROR_DEVICE_REMOVED`, logs the
removed-reason HRESULT, tears down device-dependent objects, rebuilds them, and
re-uploads from the still-live immutable `TreeResult`. Geometry is never
regenerated in this path — that is asserted by hash comparison in debug builds.

## 9. Risk register

| # | Risk | Severity | Mitigation | Status |
|---|---|---|---|---|
| R1 | Junction meshing produces cracks or non-manifold edges at high-valence unions | High | Ring-correspondence stitching + `mesh_validate` edge-manifold test gates finalisation; adversarial fixture with 6-child union | **Mitigated at module level; fixture passing. Not yet integrated into `tree_skin`** |
| R2 | Bark subdivision explodes triangle count beyond memory | High | Per-organ adaptive budget from screen-independent world-space feature size; hard cap with honest failure | Open, gated |
| R3 | Determinism lost through parallel stages | High | Per-item ID-keyed RNG substreams; determinism test runs both serial and parallel | Open, gated |
| R4 | Leaf count × leaf triangles makes mature broadleaf infeasible at reference quality | High | Measure first; profile-driven leaf triangle budget; report honestly rather than silently thinning foliage | Open |
| R5 | D3D12 layer cannot be compiled or run in the development sandbox | **Certain** | Isolate to Layer 3; verify Layers 0–2 exhaustively on host; state truthfully as unverified until user builds on Windows | **Accepted and disclosed** |
| R6 | Reverse-Z + infinite far plane misconfiguration silently breaks depth | Medium | Depth-debug view + explicit projection unit tests asserting near→1, far→0 | Open, gated |
| R7 | Mechanics solve produces unstable or exploding deflection at extreme age | Medium | Bounded per-segment rotation, curvature clamp, NaN guard, stress test at max age | Open, gated |
| R8 | Watertightness incompatible with peeling bark shells | Medium | Peeling shells are a separate, explicitly non-manifold mesh section tagged `MESH_SECTION_BARK_FLAKE`; the woody section alone is required watertight | Resolved by design |
| R9 | Host-verified float results differ from MSVC results, masking a real bug | Medium | Host tests assert invariants and tolerances, not magic constants; hashes are compared within a run, not across platforms | Resolved by design |

## 9a. R1 in detail, because it was the hardest thing in the geometry

`mesh_junction` implements the construction `docs/research.md` section 3 fixes: a
smooth-union field over the participating tapered cones, a manifold patch extracted
from it, and ring-correspondence stitching to the limb cross-sections. The mandated
adversarial fixture -- a six-child union, eight limbs meeting at a point -- is
`test_six_child_whorl`, and it asserts what the mitigation promised: one closed
component, zero boundary edges, positive enclosed volume, and the validator's
edge-manifold test passing.

One design choice was made here rather than in the research document, and it was made
against R1 itself. **Marching tetrahedra, not marching cubes.** R1's named failure mode
is non-manifold edges at high valence; marching cubes has ambiguous face and interior
cases whose mishandling is the textbook cause of precisely that, and handling them
correctly requires a 256-entry table long enough to mistype silently. A tetrahedral
decomposition has no ambiguous case and no table, and because every extracted vertex
lies on a grid EDGE and is shared by every tetrahedron using that edge, each interior
edge of the output is used by exactly two triangles by construction rather than by
argument. Kuhn's decomposition is used so that neighbouring cells agree on the diagonal
of a shared face, which is what makes the patch watertight across cells.

Two PRECONDITIONS emerged from measurement and are now part of the module's contract,
exposed as functions the caller must consult:

- `mesh_junction_min_limb_length` -- limbs that have not yet parted company at the
  clip radius have no separate exit to sew to. A child leaving at a shallow angle is
  genuinely fused to its parent for a long way. The module refuses such a union and
  reports `not_separable` rather than emitting a ring nothing reaches.
- `mesh_junction_min_grid` -- a union meshed on a grid coarser than its thinnest limb
  produces stray components. The module raises the grid to what it needs.

**What remains for integration.** `tree_skin` does not call this yet, and the reason is
specific: the junction region of a typical union extends about 2.3 parent radii, while
successive nodes on a branch are about 1.5 radii apart, so junction regions on a real
tree OVERLAP. Two overlapping unions cannot be welded independently. The module already
accepts up to twelve limbs at one union, so the integration is a clustering problem --
group nearby children into a single multi-limb junction -- not a limitation of the
welding. That clustering is the next piece of work and is recorded in
`docs/limitations.md`.

## 10. Directory layout

```
build-and-run.cmd          Windows build+run (safe, vswhere-based)
tools/host-build.sh        Host verification build for Layers 0-2 + tests
docs/                      research, architecture, tree-profiles, rendering,
                           testing, controls, limitations
src/core/                  Layer 0
src/geom/                  Layer 1
src/tree/                  Layer 2
src/render/                Layer 3  [Windows]
src/app/                   Layer 4  [Windows]
src/shaders/               HLSL
tests/                     portable test suite
tools/refgen.c             headless generator: OBJ + validation captures
tools/swrast.c             CPU rasteriser used ONLY for validation captures
```

`tools/swrast.c` is a test instrument, not a renderer alternative. It is excluded
from the shipped application build and is only invoked by the validation harness
so that generated geometry can be visually inspected without a GPU.
