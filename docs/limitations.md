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

- Geometry layer: nothing. `mesh_junction` completes the layer as a module; what is
  outstanding is its INTEGRATION into `tree_skin`, so the trees the generator produces
  still carry interpenetrating tubes at their unions. See section 3b.
- Tree layer: dormant buds and bud scales, leaf veins and midrib relief, damage.
- Render layer: Win32 platform, D3D12 device, raster PBR renderer, progressive
  renderer, lighting, picking.
- App layer: UI, construction replay controls, inspection panels. The engine side of
  both now exists -- a per-step construction record and a ray-to-organ inspection
  query -- so what is missing is the interface, not the data.
- HLSL shaders.
- `build-and-run.cmd`.

Now implemented, having been on this list: surface skinning (`tree_skin`), leaves
and needles as real geometry (`tree_foliage`), bark relief as real geometry
(`tree_bark`), build orchestration and the construction stage record (`tree_build`),
the mesh BVH (`mesh_bvh`), geometry inspection (`tree_inspect`), the headless
reference generator (`tools/refgen.c`) and the validation-capture rasteriser
(`tools/swrast.c`).

## 3. Implemented and verified

**Layer 0 (core)** — `core_types.h`, `log`, `mem`, `math3d`, `hash`, `rng`.

**Layer 1 (geom), complete** — `mesh`, `mesh_validate`, `spatial`, `camera`,
`mesh_bvh`, `mesh_junction`.

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

| Quantity | Broadleaf 80 yr | Conifer 80 yr | Broadleaf 220 yr |
|---|---|---|---|
| Growth steps completed | 80 of 80 | 80 of 80 | 220 of 220 |
| Realised height | 18.93 m (target 18.79) | 26.88 m (target 27.29) | 25.16 m (target 24.99) |
| Organs / axes | 492 361 / 86 559 | 776 648 / 92 738 | 1 190 126 / 169 542 |
| Shed by shade / by recession | 14 050 / 36 | 0 / 1 344 | 53 918 / 91 |
| Max branch order | 8 | 3 | 8 |
| Wood triangles | 10 700 000 | 16 838 264 | 20 500 000 |
| Foliage placed | 191 215 of 191 215 (100%) | 799 317 of 11 866 773 (6.7%) | 250 196 of 346 595 (72%) |
| Leaf area on the geometry | 606.8 m2 | 29.0 m2 | 793.2 m2 |
| Mesh | VALID, 0 boundary edges | VALID, 0 boundary edges | too large to validate |
| CPU geometry | 851 MiB | 1 038 MiB | 1 617 MiB |
| Generation time | ~3 s | ~4 s | ~9 s |

Realised height now matches the resolved target to within 1-2% on all three,
because the leader is driven BY the height curve rather than coincidentally
agreeing with it. The 220-year individual completes its full history for the first
time.

606.8 m2 of leaf on an 18.9 m broadleaf is inside the 300-600 m2 range a mature
open-grown oak shows. Over a crown 19.19 m wide that is a leaf area index near 2.1,
against the 4-6 of a closed crown -- so the leaf area is right and the crown is
wide. Whether the crown/height ratio in the profile is too generous is an open
question and is listed below rather than quietly adjusted.

### Foliage: what is real and what is budgeted

Every leaf, petiole and needle is indexed triangle geometry. There are no alpha
cards, no billboards and no textured quads anywhere: a blade is a lobed, cupped,
drooping shell WITH THICKNESS and a distinguishable upper and lower face, and a
needle is a closed tapered prism. The mesh validator confirms it rather than the
comment: the leaf section has zero boundary edges, positive enclosed volume, and
exactly two closed components per leaf (blade plus petiole). A card cannot pass any
of those three.

Three defects were found and fixed here, all of them by the validator or by
measurement rather than by inspection:

- **The mechanics and the geometry disagreed about what a leaf's area is.** The
  mechanics assumed 0.65 of a blade's bounding rectangle; the generated blade
  measured 0.25 of it. The tree was therefore bending under two and a half times
  the foliage it contained. The area is now INTEGRATED from the same half-width
  function the mesh is built from, at the same station count, so the two cannot
  drift apart; a test asserts they agree within 8%, and they agree within 1%.
- **The margin was aliased, not tessellated.** A five-lobed blade sampled at three
  spanwise stations put its one interior sample in a sinus and came out with 15
  times too little area. The margin frequency is now capped by the sample rate --
  the same rule that governs ring segments around a branch -- so coarse blades lose
  lobes instead of gaining noise.
- **The blade wound inward.** 151 inverted components on a 151-leaf tree, which is
  how a systematic winding error announces itself rather than a stray triangle.

What IS budgeted is the count. An 80-year broadleaf wants 191 215 leaves and gets
all of them at draft quality. An 80-year conifer wants 11 866 773 needles, which is
not an error -- a real spruce carries tens of millions -- and 11.9 million needles
is 71 million triangles. It gets 6.7% of them. Thinning is uniform over the crown
(by a hash of each leaf's identity, so that graph order, which is acropetal, cannot
leave the outer crown bald -- a test measures the placed fraction in each half of
the crown), leaf SIZE is never inflated to compensate, and both numbers are printed
on every run.

The visible consequence is specific and worth stating: at 6.7% needle cover the
conifer's silhouette is dominated by its own twigs rather than by foliage, so it
renders pale grey-brown instead of dark green. For an evergreen the twigs are
completely hidden in reality, which points at the fix -- an evergreen should spend
far less of its budget on wood and far more on needles, because its wood is
occluded. That rebalancing is not done.

### 3b. Welded branch unions: the module is proven, the integration is not

`docs/research.md` section 3 calls junction geometry "the single most important realism
decision" and rejects two alternatives by name: boolean CSG, and pushing one cylinder
through another. The second is exactly what the generator has been doing -- 82,437
interpenetrating tubes on an 80-year broadleaf -- and it is what `mesh_junction` exists
to replace.

**What is proven.** The module takes the limbs meeting at a union, each with its
already-emitted boundary ring, and returns a single welded manifold: a smooth-union
field over tapered cones, a patch extracted by marching tetrahedra, and
ring-correspondence stitching to the rings. Risk R1's mandated adversarial fixture, a
six-child union of eight limbs, produces **one closed component, zero boundary edges,
positive enclosed volume, and passes the validator's edge-manifold test**. So does every
valence from one to six children, every ring resolution from 6 to 64 segments, and 45
combinations of radius ratio, insertion angle and valence in the suite -- plus a
270-configuration sweep run outside it. Enclosed volume is bounded above and below by
independent estimates, and geometry is byte-identical across clang and gcc in debug and
release.

**Seven defects, and every one is now a test.** Six of the seven were caught by the mesh
validator rather than by reading the code, which is the argument for gating on it:

1. A **zero-width seam**. With the patch cut exactly at the ring plane, the two loops
   being sewn lay on the same circle in the same plane, so every seam triangle was a
   sliver with an undefined normal: 62 inconsistent windings and 20 boundary edges. The
   patch is now cut short of the ring by a seam band with real width.
2. **Per-triangle winding from the field gradient.** Adjacent triangles share a diagonal
   and must traverse it in opposite directions; an independent per-triangle test cannot
   guarantee that. Winding is now derived from which side of the surface is inside,
   which is exact, local and automatically consistent between neighbouring tetrahedra.
3. **A seam that sorted the loop by angle.** That silently discarded the loop's
   connectivity, so the patch's own boundary edges went unpaired wherever the two orders
   disagreed: 26 boundary edges on one union. The loop is now used exactly as it was
   walked.
4. **Plane-plane corners in the clip region.** Clipping with one half-space per limb
   manufactured edges belonging to no limb's exit where two planes met: nine boundary
   loops for seven limbs, two of them unsewn. Clipping with a ball has no corners.
5. **A ragged cap-classification band.** Dropping triangles within a fraction of a cell
   of the clip surface is right for some orientations and wrong for others; ragged
   dropping left isolated holes and twelve loops on a three-limb union. Classification is
   now by which field term is active, which is a partition and cannot be ragged.
6. **A boundary walk keyed by vertex.** That assumes the boundary is a union of simple
   cycles, which fails where the surface pinches to a point. The walk now follows the
   triangle fan, which disambiguates correctly at a pinch. A pinch cannot be repaired by
   duplicating the vertex, because the validator welds by exact position and would weld
   it straight back; so a pinch that survives is detected and the extraction is retried
   with a nudged clip radius, deliberately at fractions incommensurate with the cell
   size.
7. **A merge advanced by angle.** If all of one loop's angles happen to precede the
   other's, that side saturates, its index wraps to its first vertex, the starting pair
   is visited twice and its diagonal is emitted twice -- one edge used by four triangles,
   on a union with no duplicate vertices and no pinch anywhere. Advance is now
   proportional to the two vertex counts, in integer arithmetic.

**Two preconditions, computed rather than assumed.** Both are exposed as functions the
caller must consult, and the module refuses rather than cracking when they are unmet:
`mesh_junction_min_limb_length`, because limbs that have not parted company at the clip
radius have no separate exit to sew to -- a child leaving at 0.2 rad is genuinely fused
to its parent for 1.97 m; and `mesh_junction_min_grid`, because a union meshed coarser
than its thinnest limb produces stray components.

**What is NOT done, and the specific reason.** `tree_skin` does not call this yet. The
junction region of a typical union extends about 2.3 parent radii, while successive
nodes on a branch are about 1.5 radii apart, so on a real tree the junction regions
OVERLAP -- and two overlapping unions cannot be welded independently. The module already
accepts twelve limbs at one union, so this is a clustering problem, not a limitation of
the welding: nearby children have to be grouped into a single multi-limb junction, and
the cluster's limb length and clip radius derived from the group. Until that exists,
generated trees still report `interpenetrating_unions` and the claim "the tree is one
solid" is **not** made.

Cost, measured: a single union at grid 32 with eight limbs is about 12,200 patch
triangles and 600 seam triangles. That is affordable only for unions coarse enough to be
worth welding, which is a further reason the integration needs a visibility threshold as
well as a clustering pass.

### Bark: relief as geometry, and what it took to see it

Bark is a radial DISPLACEMENT of the wood tube's own rings, not a second shell and
not a normal map. The directive forbids the map, and a map fails in three specific
ways no texture resolution fixes: the silhouette stays a smooth cylinder, grazing
light produces no real occlusion in the furrows, and there is no parallax so the
ridges slide across the surface as the camera moves. A furrow 27 mm deep in the mesh
does all three for free, and it appears in the silhouette, which is checkable.

The field is hash-based value noise on a lattice that wraps at an INTEGER number of
cells around the axis, terraced into flat crests and flat floors joined by steep
walls, sheared so the ridges interlace rather than running as parallel stripes, and
keyed longitudinally to absolute arc length so a ridge crosses internode boundaries
unbroken. Material and ambient occlusion both follow the displacement, so the
shading and the geometry cannot disagree.

Four defects, each of which cost a render cycle to find and now has a test:

- **The relief was there and invisible.** Maturity was taken from a binary material
  flag, which returned BARK_YOUNG for the trunk, so every furrow was exactly zero
  deep. A test now asserts that a 90-year trunk reports maturity above 0.5 and
  peak-to-trough relief above 10 mm.
- **The sample rate resolved the lattice, not the feature.** Two and a half samples
  per cell is over two per lattice cell but under ONE per furrow, because the
  shaping function confines a furrow to about a third of a cell. The rendered trunk
  was smooth while carrying 27 mm of displacement. Now seven samples per cell
  around and five along, with a test that checks the ratio.
- **A power curve is not bark.** It gives a C1-smooth surface and the trunk read as
  gentle vertical undulation. Terracing -- flat crest, steep wall, flat floor --
  is what makes it read as split rhytidome.
- **The field was not periodic.** A coarser octave sampled at 0.35x the base
  coordinate with a period of cells/3 does not wrap: 13.9 mm of discontinuity down
  the length of every trunk. The integer cell count is now chosen first and the
  coordinate scale derived from it. The periodicity test found this in one run.

Fissure scale follows radius as its square root, because the profile's feature size
describes the trunk base and a primary limb is finely fissured where the bole is
deeply ridged. With a fixed 6 cm feature only THREE axes of 82,437 were thick enough
to carry relief at all.

Relief is gated at 35 mm of radius, which on an 80-year broadleaf selects 22 axes of
82,437 -- the bole and the major limbs. That is deliberate on both counts: a 3 cm
oak branch is genuinely smooth, and restricting relief to the surfaces anybody
inspects is what pays for sampling them well enough to see. At a 15 mm threshold the
same tree put relief on 111 axes and cost 2.1 GB.

### Construction replay: the data exists, the viewer does not

Every mesh vertex now carries the growth step at which its organ came into being, in
the slot that was explicit padding, so the stride is unchanged at 64 bytes. This is
what makes the replay a CLIP TEST rather than a rebuild: revealing an eighty-step
history by regenerating the tree at each step would cost eighty full generations, and
worse, it would not be provably the same tree at each stage. Nothing consumes this
yet -- the replay is a renderer and UI feature and neither exists.

### Build orchestration, spatial index and inspection

Three things landed together because they are the engine-side of two of the
directive's four pillars, and neither pillar had anything at all.

**One entry point.** `tree_build` runs resolve, grow shoots, grow roots, mechanics,
skin, foliage, finalise, validate, index and construction record, in that order, with
progress reporting and cancellation, and returns everything in one struct with one
free function. The order was previously hand-wired by every caller, and getting it
wrong is SILENT: skinning before the mechanics pass produces a complete, watertight,
validated mesh of an unbent tree with no radii, and nothing downstream complains.
The reference generator now uses it, so there is one copy rather than two. Tests
assert the properties the order exists to guarantee rather than the order itself.

**A static BVH** over the finalised mesh, giving pick queries in log time. Measured
on the 80-year broadleaf: 4.19 million nodes over 9.88 million wood triangles, depth
21, 4.7 triangles per leaf, 215 MiB, and a pick that tests 54 triangles out of ten
million. Every one of 240 randomly aimed rays in the test suite is checked against
an exhaustive scan of every triangle -- not against a recorded expectation, which
would only prove the code still does what it did.

Three things were got wrong here and are worth recording:

- **The node bound was wrong and a young tree failed to build.** A median split only
  occurs above the leaf target, so a node of five triangles splits into two and three
  and both become leaves; leaves hold as few as half a target and the node count can
  approach the triangle count. The array now grows, and every access is by index
  rather than by cached pointer precisely so a growth mid-build is safe.
- **The index was sized from the whole mesh, not from the sections it covers.** A
  wood-only index over a tree whose foliage is two thirds of its triangles allocated
  400 MB where 230 was used. The section mask exists so the index is smaller than the
  mesh; the allocation has to honour it.
- **A leaf of four cost 448 MiB.** Eight halves the node count for roughly twice the
  triangle tests per query -- 54 rising to about 100 out of ten million, which is not
  a cost worth 230 MB.

**Inspection.** `tree_inspect_ray` turns a ray into the full biological record of
what it hit: organ, order, axis and axis kind, the year it was formed, cambial age,
length and radii, the leaf area and wood mass it carries, its path length back to the
base and how many organs deep it is -- plus the cost of the query itself, so a slow
pick is measurable rather than suspected. `tree_inspect_describe` formats it. Every
reported fact is checked against the graph it came from in the test suite, over
whatever subset of 120 rays happens to hit.

This is what makes a wrong tree diagnosable. "The crown is too sparse" is an opinion;
"this is order 7, formed in year 74, 1.9 mm thick, carrying 0.004 m2 of leaf" is a
measurement. Every reference capture now prints one worked inspection.

**The construction record**, derived from the finished graph rather than captured
during simulation, so a replay narrates the tree that is actually on screen. It
immediately found a real defect: a twelve-year tree reported 141 organs already in
existence at year 0, because the root system is generated in one pass after the
shoots and every root organ was stamped with step 0 -- the whole root plate appearing
fully formed beneath a seedling. Root birth steps are now inferred from fractional
position along the root axis, which is an inference from the shoot growth curve rather
than a simulated root growth model and is labelled as one. The same tree now reports
3 organs at year 0, 135 at year 6 and 392 at year 12.

That defect also exposed a gap in the fingerprint: it did not fold in the per-vertex
birth step, so moving an entire root plate from year 0 to a plausible progression
produced a bit-identical fingerprint and the regression gate would have reported that
nothing had changed. It is folded in now.

### Determinism, measured across compilers

Four builds -- clang and gcc, `-O0 -DTG_DEBUG=1` and `-O2 -DNDEBUG` -- produce
BYTE-IDENTICAL tree fingerprints, organ counts, axis counts, vertex counts, triangle
counts and BVH shapes on four different trees. The probe is
`build-host/diag/fp.c`.

One caveat found while establishing this, and it is about the tests rather than the
engine: clang and gcc disagreed on whether three of 120 grazing rays hit a triangle,
which is ordinary floating-point rounding in the intersection test. That is not a
generation difference -- no ray casting happens during generation -- but with the
assertions inside the ray loop it changed the number of CHECKS the suite ran, from
697,685 to 697,718, and that count is one of the signals used to notice unintended
behaviour change. The ray tests now accumulate and assert once, so the suite's own
shape does not depend on rounding.

### Remaining defects, stated plainly

1. **Conifer self-shading mortality is zero.** Not merely low: no conifer shoot
   dies of shade. 41 056 shoots sit pinned at the crown surface and none is ever
   overtopped. Its lower crown is now shed by crown recession (1 344 events)
   instead, which cleans the skirt but is not the same mechanism.
2. **Crown recession is prescribed, not emergent.** A shoot whose base falls below
   the live crown base is shed. That is a real phenomenon and the live crown base
   is a resolved property of the individual, but the shedding is imposed rather
   than arising from shade -- because a low branch sits at the crown SURFACE, where
   it is well lit, so the transmittance model quite correctly spares it. Before the
   sweep existed the 80-year broadleaf carried its crown to within 2 m of the
   ground against a resolved crown base of 5.26 m.
3. **The conifer's upper leader is too bare.** The leader tracks the height curve
   and outruns its own laterals, leaving roughly the top fifth an almost naked
   spire.
4. **The crown/height ratio may be too generous.** 19.19 m of crown on an 18.93 m
   broadleaf gives a leaf area index of 2.1 where a closed crown shows 4-6. The
   leaf area itself is correct, so either the crown is too wide or the tree should
   carry more foliage-bearing shoot. Not adjusted, because guessing which would be
   fitting one number by breaking another.
5. **Mesh size is far beyond a real-time budget.** 10.7 to 20.5 million wood
   triangles per tree at 851-1 617 MiB. Every branch is still a separate closed
   tube, so a large fraction of those triangles are inside other triangles.
   `mesh_junction` (welding unions) and a mesh-level LOD are the next work, and
   until they exist the triangle count should be read as an upper bound.
6. **Mesh validation cannot run on the largest tree.** The 220-year case exceeds
   the topology scratch buffers. It is now reported as `NOT VALIDATED` rather than
   `INVALID`, which was the previous behaviour and conflated "failed the checks"
   with "was never checked".
7. **Very old individuals no longer truncate, but only just.** The 220-year tree
   completes all 220 steps at 1 190 126 organs against a 1 400 000 ceiling. It has
   no crown retrenchment, so nothing bounds the accumulation from above; a
   400-year individual would truncate again.
8. **Quality is not a pure level of detail.** `internode_geometry_scale` groups
   botanical nodes into geometric internodes, and a shoot whose annual increment
   does not complete one geometric internode produces no laterals that year, so a
   draft tree branches slightly less than a reference tree at the finest orders.
9. **Bark relief is smooth-edged and lacks secondary detail.** It reads as
   furrowed rhytidome but the ridges are rounded rather than cracked, and there are
   no secondary fissures across a plate, no lenticels, no branch-scar collars and no
   change of character at the buttress. The terracing gives a crease at the top of
   each wall; genuinely fractured bark needs a second, finer field and the sampling
   to carry it.
10. **The conifer still reads as speckle at 22% needle coverage.** An evergreen now
   gets twice the foliage budget and coarser wood -- justified by occlusion, since a
   conifer's shoots are completely clothed and its wood invisible -- which took
   coverage from 10.5% to 22.2% and 3.0 million needles. It is visibly better and
   still not a spruce: at close range the naked pale twigs are what dominates.
   Reaching full coverage means 13.5 million needles, 81 million triangles and about
   4.4 GB, which is beyond what a single tree can be given.
11. **The conifer mesh now exceeds the topology scratch limit too**, for the same
   reason the 220-year broadleaf does, and is reported as NOT VALIDATED.
12. **The BVH is a median split, not a surface-area heuristic.** Adequate for
   geometry as uniformly distributed as a tree's surface, and the cost achieved is
   reported rather than assumed, but a proper SAH build would give shallower trees
   and fewer triangle tests.
13. **Root growth is inferred, not simulated.** Root birth steps come from position
   along the root axis mapped onto the shoot growth curve. Real roots respond to
   soil, water and obstruction on their own schedule.
14. **Region inspection samples rather than enumerates.** `tree_inspect_region`
   examines up to 4096 triangles and counts distinct organs with a one-element memo
   that relies on triangles arriving grouped by organ. Both are stated in the source
   and the truncation is reported to the caller.
15. **Leaf veins and midrib relief do not exist.** The blade is a smooth shell. The
   relief belongs in a leaf-detail pass that displaces this surface at high
   quality; an earlier attempt to carry a midrib in the blade's TOPOLOGY produced
   906 boundary edges and 1 057 non-manifold edges, which is the wrong place for
   it.

### What was checked visually, and what it showed

The CPU rasteriser was the instrument that made all of the above findable. The
first captures after the flush rewrite showed a bare pole under a flat pancake of
twigs -- a result that every numeric check had passed. Since then it has also caught the lozenge-shaped crown
envelope (fixed by replacing a pair of plain powers with super-ellipse quadrants,
which gave the broadleaf the flattened dome it should have and left the conifer its
spire), the crown reaching to within 2 m of the ground, and the pale conifer.

The current captures show a dense rounded crown of real leaves over a visible bole
and root plate for the broadleaf, and a clean cone with a single leader for the
conifer, with the defects listed above visible on inspection.

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
