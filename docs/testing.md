# Test and Validation Plan

## 1. Two build paths, and what each one proves

| Build | Command | Covers | Proves |
|---|---|---|---|
| Host verification | `tools/host-build.sh [debug\|release\|asan]` | Layers 0–2 + `tests/` | determinism, mesh validity, invariants, memory discipline, numerical behaviour |
| Windows product | `build-and-run.cmd` | all layers + HLSL | that the application builds, runs, and displays |

The host path is not a convenience. It is the only way to prove the geometry
contract without a GPU, and it is also the layering enforcement mechanism: it
compiles the portable layers with no Windows headers present, so a stray
platform dependency fails the build immediately.

## 2. Gate order

A change may not advance until every applicable gate passes.

1. **Baseline** — read the affected code, run the existing suite, record the
   result, create the git checkpoint.
2. **Compile clean** — both compilers available on the host, `-Werror`, debug and
   release. `/W4 /WX` on MSVC.
3. **Unit** — the portable suite.
4. **Determinism** — identical fingerprints for identical inputs, serial and
   parallel.
5. **Mesh validity** — watertightness, manifoldness, winding, radius invariants.
6. **Static-object regression** — geometry fingerprint unchanged across camera,
   light, UI, resize, and long-session activity.
7. **Adversarial** — allocation failure, extreme age, degenerate input, zero and
   maximum counts, cancellation, repeated regeneration.
8. **Visual** — reference captures reviewed, not auto-accepted.
9. **Integration** — real main-loop sequencing, device loss, shutdown.

## 3. Current suite

```
tests/test_mem.c    arena, mark/restore, dynamic array, checked arithmetic,
                    overflow refusal, injected allocation failure, leak gate
tests/test_math.c   scalars, vectors, quaternions, matrices, look-at,
                    reverse-Z projections, rotation-minimising frames, AABB,
                    ray/triangle, ray/AABB
tests/test_rng.c    stream determinism, substream order-independence,
                    distribution bias, unbiased integer range, normal moments,
                    sphere/ball/cone sampling correctness
tests/test_hash.c   avalanche, spread over sequential integers, geometry
                    fingerprint sensitivity, signed-zero folding, non-finite
                    detection, one-ulp mutation detection
tests/test_mesh.c   vertex/attrib packing, section discipline, closed-solid
                    validation against independent analytic volumes, hard-edge
                    and watertightness coexistence, near-miss crack detection,
                    hole/winding/duplicate/inversion detection, per-component
                    inversion among many components, numeric corruption,
                    scale-relative area threshold, fingerprint behaviour,
                    limits, empty mesh
tests/test_mesh_junction.c  the risk register's mandated six-child adversarial
                    fixture, every valence from one to six children, ring
                    resolutions from 6 to 64 segments, a grid deliberately too
                    coarse, a shallow-angle union refused and then welded once
                    given the length it asked for, seven classes of malformed
                    spec, field sign structure and the measurable existence of
                    the collar fillet, enclosed volume between two independent
                    bounds, byte-identical determinism, organ and birth-step
                    attribution of the patch, and a 45-configuration sweep of
                    radius ratio, insertion angle and valence
tests/test_spatial.c grid queries checked against a brute-force reference,
                    ascending-order guarantee, bounded-output determinism,
                    cell-size invariance, removal without rebuild, degenerate
                    distributions, bad input rejection
tests/test_tree_profile.c  built-in profile coherence, 18 negative tests for the
                    directive's forbidden botanical combinations, correlated
                    resolution (age, wind, canopy, soil, health, season), bounded
                    seed individuality, quality budgets that change resolution
                    only, crown envelope shape per category
tests/test_tree_graph.c  ordering invariant, RMF frame propagation and continuity
                    across unions, 14 corruption-detection cases, radius
                    invariant with its deformity escape, finalisation
                    immutability, subtree mortality, basipetal accumulation on a
                    30 000-segment chain, fingerprint sensitivity
tests/test_tree_growth.c  attractor cloud placement and calibration, full-tree
                    validation, bit-identical determinism, per-setting
                    sensitivity, broadleaf/conifer architectural distinction via
                    reiteration, roots not mirrored branches, shade mortality
                    with a measured height bias, directional crown asymmetry with
                    an isotropic control, branch orders not scaled copies,
                    sibling inequality from the resource partition, age
                    progression, cancellation, organ-budget reservation
tests/test_tree_mechanics.c  radius assignment and both radius invariants,
                    basal flare presence and decay, the pipe relation verified
                    directly against r = k*A^(1/delta), exponent sensitivity,
                    mass against an independent bounding-cylinder estimate,
                    evergreen vs deciduous foliage load, winter fallback taper,
                    deflection against an unbent baseline, the own-bend sign
                    audit, joint connectivity after bending, frame orthonormality
                    after bending, stiffness and retention sensitivity, reaction
                    wood correlated with load, determinism, non-no-op check, age
                    scaling, independent root sizing and taper
tests/test_camera.c FOV clamping, basis orthonormality, pitch clamp with no
                    gimbal flip, orbit leaves pivot untouched, framing from any
                    angle and aspect, visibility-predicate anti-vacuity,
                    scale-adaptive speed and clipping, minimised-window aspect,
                    pick-ray orientation and FOV agreement, mode switching
                    without a jump, reverse-Z depth ordering, 100k-interaction
                    long-session stability
```

### Check-count stability

The harness prints a per-suite check count. That count is required to be
**data independent**: any assertion whose execution depends on a floating-point
comparison result is restructured into an aggregate assertion. The reason is that
a point lying exactly on a query radius rounds differently under clang and gcc,
which changed how many assertions ran and turned the count into noise. With that
fixed, the entire test output is byte-identical across clang/gcc and debug/release,
which makes any change in the count a real signal.

The harness reports every failure in a run rather than aborting on the first, so
one build/run cycle gives the complete picture during regression triage.

## 4. Tests that exist specifically to catch a known failure mode

These are not generic coverage. Each one targets a defect this project would
otherwise be likely to ship.

| Test | Failure mode it catches |
|---|---|
| position-welded topology, both directions | either reporting every hard bark edge as a hole, or welding away a genuine sub-micron crack |
| per-component enclosed volume | one inverted leaf hidden by a positive section total |
| component-local volume origin | a correct leaf far from the model origin rejected because of float cancellation |
| angle-weighted normals | diagonal shading stripes from quad-triangulation bias |
| dihedral-spread hard-edge check | a 90-degree edge silently smoothed because each face deviates only 45 degrees from the bisector |
| scale-relative area threshold | legitimate sub-millimetre leaf-vein triangles rejected as degenerate |
| grid cell-size invariance | a hard-coded 3x3x3 neighbourhood silently missing attractors, which reads as poor crown quality |
| bounded-query determinism | an overflowing neighbour query returning whichever points were visited first |
| camera pitch clamp | gimbal flip when orbiting past vertical |
| framing from all angles/aspects | the object breathing as the user orbits, or clipping at extreme aspect ratios |
| framing does not change FOV | the forbidden shortcut of standing too close and widening the lens |
| pick-ray angle equals FOV/2 | picking offset from what the user clicked |
| minimised-window aspect | NaN projection persisting after restore |
| visibility-predicate anti-vacuity | a framing test suite that asserts nothing |
| axis chain walk over non-segment children | a validator that rejects every genuinely grown tree because growth interleaves axes |
| internode length tied to the height-curve derivative | growth and resolution models disagreeing; a 60-year tree came out 29.7 m against a 16.1 m target |
| light sampled ahead of the apex | every new lateral condemned at birth, giving 60% mortality with no height bias |
| directional (light-hemisphere) occlusion | counting all neighbours, so a shoot's own siblings shaded it and no second branch order ever formed |
| canopy closure entering the light field | a forest tree suffering LESS mortality than an open-grown one |
| organ-budget reservation for roots | a large conifer spending the whole ceiling above ground and generating no roots at all |
| shade mortality height bias | mortality that exists but is not actually shade driven |
| isotropic-light control case | crown asymmetry that appears without a cause |
| root/crown radius ratio | root spread scaled from height, giving a 12:1 plate on the conifer |
| conifer mortality gate | a recorded known defect silently getting worse |
| own-bend sign audit inside the pass | a deflection sign error hidden by rigid propagation, which legitimately lifts back-pointing branches |
| deflection vs an unbent baseline | comparing a bent segment against its parent, which gravitropism already makes point more upward |
| pipe relation tested directly, not at "branch points" | a test that found zero checkable cases because laterals hang off buds, and so asserted nothing |
| frame orthonormality after bending | stale rotation-minimising frames shearing every cross-section |
| joint connectivity after bending | rotating a segment without re-anchoring its children, opening a crack at every joint |
| physiological age without assuming the leader is youngest | a correct tree failing because its leader stopped extending decades ago |
| reverse-Z near→1 / far→0 and monotonicity | depth silently rebuilt as conventional Z, or sign error making everything fail the depth test |
| "reverse-Z concentrates precision near the camera" | a refactor that keeps the endpoints correct but loses the precision distribution |
| vertical-FOV assertion against `tan(fov/2)` | accidental fisheye or a factor-of-two error in the FOV |
| infinite far plane never yields negative depth | uncontrolled far clipping during inspection |
| RMF orthonormality along a curving, twisting helix | cross-sections shearing along a branch |
| RMF zero-twist on a straight axis | bark grain spiralling up a straight trunk |
| RMF out-and-back twist bound | accumulated twist over a long axis |
| RMF with a duplicated point | NaN from a zero-length internode |
| ring winding is CCW seen from +t | inside-out triangles, which read as holes |
| substream order-independence | a parallel stage changing the tree depending on thread scheduling |
| sequential ids not correlated | visible directional bias in phyllotaxis and attractor sampling |
| unbiased `below(n)` for n=7 | modulo bias skewing discrete botanical choices |
| cone sampling solid-angle uniformity | foliage and shoot directions clustering on the axis |
| signed-zero folding in the fingerprint | false "geometry changed" regression failures |
| one-ulp mutation detection | a fingerprint too weak to detect a real geometry change |
| failed array growth preserves contents | losing a valid tree on an allocation failure |
| leak gate across all suites | ownership defects |
| six-child union welds to ONE closed component | the risk register's R1: cracks or non-manifold edges at high valence, which is where a junction scheme that works on a simple bifurcation falls apart |
| seam triangle count equals the sum of the two loop lengths | a stitch that closes approximately; the count IS the watertightness argument, because it means every edge of both loops is consumed exactly once |
| seam diagonal reuse counted explicitly | a merge advanced by angle, which lets one side saturate, wrap to its first vertex and revisit its starting pair -- one edge used by four triangles on a union with no duplicate vertices and no pinch anywhere |
| boundary loop simplicity checked before anything is committed | a boundary walk keyed by vertex, which cannot represent a surface that pinches to a point; the pinch cannot be repaired by duplicating the vertex because the validator welds by exact position, so it must be avoided and retried |
| enclosed volume between limb tubes alone and limbs plus junction ball | a winding error, a lost limb, or a doubled patch, none of which changes the triangle count |
| the collar measured as a fillet in the field, not asserted | a collar added as decoration rather than produced by the smooth union, which is what research.md requires |
| inseparable union emits NOTHING | a ring left unreachable: a hole the size of a branch, reported as success |
| boundary normals perpendicular to their limb axis | normals taken from the clipped field rather than the smooth union, which is invisible to every topological check and renders as a dark sawtooth band around every seam; found by a capture, and the test was verified by reintroducing the defect |
| junction patch carries an organ id and a birth step | junction geometry that cannot be inspected, or that appears at step zero and shows a fully formed fork under a seedling |

## 5. Rules

- Never weaken a test to make it pass. Change an expectation only after
  recording why the previous requirement was wrong.
- A passing test that derives its expected value from the implementation proves
  nothing. Expectations come from mathematics, from the documented convention,
  or from an independent computation.
- Skipped tests are failures until explicitly justified in this document.

## 6. Verified state as of the current commit

Run on the development host (Linux x86-64), both available compilers:

| Configuration | Result |
|---|---|
| clang 15.0.7, release (`-O2 -DNDEBUG`) | 448 cases, 711 007 checks, 0 failures |
| gcc 11.5.0, release | 448 cases, 711 007 checks, 0 failures |
| clang 15.0.7, debug (`-O0 -g3 -DTG_DEBUG=1`) | every suite run individually, 0 failures |
| gcc 11.5.0, debug | compiles clean; `geom/junction`, `geom/mesh`, `tree/build`, `tree/bark`, `tree/foliage` run, 0 failures |

The two release runs produce **byte-identical output**, including identical per-suite
check counts.

The debug configurations are recorded as suite-by-suite rather than as one run, and the
reason is stated rather than glossed: `tree/mechanics` alone takes about eleven minutes
at `-O0`, which exceeds the single-command time limit available in this development
sandbox. Every suite was run and every suite passed; the aggregate line was not produced
in one process. gcc debug was exercised on the suites touching the code changed in this
checkpoint plus the mesh and geometry suites, not on all fourteen.

**Determinism, measured across compilers and configurations.** Four builds -- clang and
gcc, `-O0 -DTG_DEBUG=1` and `-O2 -DNDEBUG` -- produce byte-identical tree fingerprints,
organ, axis, vertex and triangle counts and BVH shapes over four different trees. The
probe is `build-host/diag/fp.c`. Fingerprints changed at this checkpoint because the
per-vertex birth step is now folded into `mesh_fingerprint`; that was a deliberate
correction, since a change to when geometry comes into existence must register as a
change to the geometry.

The run intentionally prints WARNs from `tree_mechanics` about segments hitting the
rotation clamp, from `tree_skin` about hard-edge corners and interpenetrating unions, and
from `tree_foliage` about the foliage budget. Those are known limitations surfacing
themselves on every run rather than being hidden; see `limitations.md`.

Warnings: zero, with `-Werror -Wall -Wextra -Wshadow -Wconversion
-Wdouble-promotion -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith
-Wcast-align -Wwrite-strings -Wundef -Wvla -Wswitch-enum`.

Leak gate: 0 bytes live at exit, with per-suite attribution.

**Gate 8, visual.** A welded union was rendered and reviewed rather than accepted on the
strength of its validation report, and that review found a defect no topological check
could see: see defect 8 in `limitations.md`. Captures are produced by
`build-host/diag/junc.c` into `artifacts/junction_*.png`.

**Not verified here:** the `asan` mode is authored and compiles, but the
AddressSanitizer and UndefinedBehaviorSanitizer runtime libraries are not installed in
the development sandbox, so it cannot be executed. See `limitations.md`.
