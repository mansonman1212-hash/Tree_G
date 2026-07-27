# Biological and Computational Research Record

Status: Phase 0 gate document. Every model choice recorded here is binding on the
implementation. Where a choice is an approximation, the approximation and its
visible consequence are stated explicitly. Nothing in this document may be
described elsewhere in the project as "physically accurate" unless this document
says so.

Sources are cited inline. All source content is paraphrased; no source is quoted
at length. Content was rephrased for compliance with licensing restrictions.

---

## 1. Scope of the biological model

The engine generates **one** individual tree from two initial botanical
categories:

| Category | Reference form | Architectural class |
|---|---|---|
| Broadleaf | Open-grown temperate deciduous, *Quercus*-like (pedunculate/white-oak habit) | Rauh-type: monopodial, rhythmic growth, orthotropic trunk, plagiotropic-to-ascending laterals, decurrent with age |
| Conifer | Temperate evergreen *Abies*/*Picea*-like (fir/spruce habit) | Massart-type: monopodial, rhythmic, strong apical control, whorled plagiotropic branches, excurrent |

The Hallé–Oldeman–Tomlinson architectural classification reduces tree form to a
small set of inherited developmental models defined by trunk formation,
branching rhythm, orthotropy versus plagiotropy, and the position of
reproductive structures ([Hallé, Oldeman & Tomlinson, *Tropical Trees and
Forests: An Architectural Analysis*, Springer 1978, ch. "Inherited Tree
Architecture"](https://link.springer.com/chapter/10.1007/978-3-642-81190-6_3); the
same authors also treat departures from the ideal model under "Opportunistic
Tree Architecture", [ibid. ch. 4](https://link.springer.com/chapter/10.1007/978-3-642-81190-6_4)).
Their framework is used here for one specific purpose: it fixes *which
developmental rules are legal for a profile*, preventing the engine from mixing
excurrent conifer whorls with decurrent broadleaf reiteration.

**Approximation.** We implement two architectural classes, not twenty-three. The
profile system is data-driven so further classes can be added without engine
changes, but only Rauh-like and Massart-like behaviour is validated.

### 1.1 Recorded profile decisions

| Property | Broadleaf profile | Conifer profile |
|---|---|---|
| Trunk growth | Monopodial while young; apical control decays with age, producing forking and a decurrent crown | Monopodial throughout; strong apical control retained |
| Branching rhythm | Rhythmic, one to two growth flushes per season | Rhythmic, one flush; pseudo-whorls at each annual node |
| Phyllotaxis | Alternate spiral, divergence 137.507764° (2/5 and 3/8 approximants permitted per profile) | Spiral on long shoots; needles inserted individually around the axis |
| Lateral orientation | Ascending to horizontal; older primaries sag and re-ascend at the tip | Plagiotropic, near-horizontal, distally drooping in *Picea*-like variant |
| Leaf form | Simple, pinnately lobed, 5–7 rounded lobes per side, obtuse sinuses, short petiole, subcordate/auriculate base | Linear needle, flattened rhombic cross-section, single medial groove, sessile on a raised pulvinus |
| Crown envelope | Broad, wider than tall at maturity; radius peaks at 0.45–0.60 of height | Narrow conical; radius decreases monotonically toward apex |
| Bark family | Age-dependent: smooth → shallow-fissured → deeply furrowed with interlacing vertical ridges and blocky plates | Age-dependent: smooth resinous → scaly → thick plated with resin blisters |
| Root architecture | Heart-to-taproot; strong flare, 4–7 major laterals, sinker roots | Plate/shallow-lateral; wide flat lateral spread, weak taproot in mature form |
| Reaction wood | Tension wood on the upper side | Compression wood on the lower side |

The reaction-wood asymmetry is not cosmetic. In angiosperms, reaction wood forms
on the upper side of a leaning axis and acts in tension; in gymnosperms it forms
on the lower side and acts in compression, and in both groups its presence is
visible externally as eccentric radial growth and an off-centre pith
([Penn State Extension, *Reaction Wood in Trees*](https://extension.psu.edu/reaction-wood-in-trees/);
[Clair et al., *Critical review on the mechanisms of maturation stress
generation in trees*, J. R. Soc. Interface 2016](https://royalsocietypublishing.org/doi/10.1098/rsif.2016.0550)).
Measurements on broadleaf branches report compressive strain predominantly on
the lower branch side, with branch pith eccentricity opposite in pattern to that
of tilted trunks — branch radial growth being hypotropic while inclined trunks
grow epitropically ([Yoshida et al., *Biomechanical features of eccentric cambial
growth and reaction wood formation in broadleaf tree branches*, Trees
2012](https://link.springer.com/article/10.1007/s00468-012-0733-4)).

**Implementation consequence.** Cross-sections are generated with a signed
radial eccentricity term whose sign is taken from the profile
(`REACTION_TENSION_UPPER` vs `REACTION_COMPRESSION_LOWER`) and whose magnitude
scales with the local gravitational bending moment. This produces oval branch
cross-sections and an off-centre axis, which is what an observer actually sees.

---

## 2. Growth model: a deliberate hybrid

No single published algorithm is treated as the biological answer. Each
algorithm is assigned one responsibility and is constrained by the others.

### 2.1 Layer A — Developmental grammar (architecture and identity)

Responsibility: what nodes, internodes, buds, and axis categories may exist;
rhythmic flush structure; monopodial versus sympodial continuation; phyllotactic
placement; whorl counts; bud fate categories.

Basis: parametric L-system / developmental plant modelling as formalised in *The
Algorithmic Beauty of Plants* ([Prusinkiewicz & Lindenmayer, ch. 2–3, available
from the Algorithmic Botany
archive](http://www.algorithmicbotany.org/papers/abop/abop-ch3.pdf)), and the
botanically faithful structural approach of
[de Reffye et al., SIGGRAPH 1988, *Plant models faithful to botanical structure
and development*](https://dl.acm.org/doi/10.1145/54852.378505), which explicitly
integrates growth, space occupation, and organ position rather than treating the
tree as a fractal silhouette.

We do **not** implement a string-rewriting interpreter. We implement the same
semantics directly as a typed graph production step, because the engine needs
per-organ metadata (vigor, light, supported leaf area, damage) that a symbol
string does not carry naturally. This is a representation choice, not a
simplification of the developmental rules.

### 2.2 Layer B — Bud fate and internal resource signalling

Responsibility: which buds break dormancy each growth step, how much extension
each shoot receives, apical dominance, and shade-driven abortion.

Basis: the self-organising formulation of
[Palubicki, Horel, Longay, Runions, Lane, Měch & Prusinkiewicz, *Self-organizing
tree models for image synthesis*, ACM TOG (SIGGRAPH)
2009](https://algorithmicbotany.org/papers/selforg.sig2009.html). Its central
hypothesis is that tree form emerges from buds and branches competing for light
or space, moderated by internal signalling
([paper PDF](https://www.algorithmicbotany.org/papers/selforg.sig2009.small.pdf)).
We implement the extended Borchert–Honda style basipetal/acropetal resource pass:
light-derived flux is gathered from the tips toward the base, then a fixed
resource budget is redistributed from the base outward, split at each branching
point by a profile-controlled apical/lateral partition coefficient.

**Approximation.** The signalling is a scalar flux, not auxin/cytokinin
chemistry. Consequence: correct competitive *pattern*, no hormonal transients.

### 2.3 Layer C — Space competition and crown occupation

Responsibility: direction of tip extension inside the crown envelope; production
of internal gaps and exploratory limbs.

Basis: the space colonization algorithm of
[Runions, Lane & Prusinkiewicz, *Modeling Trees with a Space Colonization
Algorithm*, EG Workshop on Natural Phenomena
2007](https://algorithmicbotany.org/papers/colonization.egwnp2007.pdf), extended
from the open leaf-venation model to three dimensions; and its fuller treatment
in [Runions' thesis (Calgary,
2008)](https://algorithmicbotany.org/papers/runionsa.th2008.html), where markers
of empty space mediate competition between branches.

Space colonization alone is explicitly rejected as sufficient (§5 of the project
directive). Here it only *steers* a tip whose existence and legality were
decided by Layer A and whose vigor was decided by Layer B.

### 2.4 Layer D — Environmental tropisms

- **Phototropism**: crown displacement toward a profile-defined light field,
  applied as a bounded per-step direction bias, accumulated across growth steps
  so the resulting curvature is segmented rather than a constant-radius arc.
- **Gravitropism**: orthotropic axes correct toward vertical; plagiotropic axes
  hold a set-point angle; roots use positive gravitropism with a distinct,
  independently parameterised response curve.
- **Thigmomorphogenesis**: a scalar historical wind exposure reduces height,
  increases basal radius, and biases branch survival on the windward side.

### 2.5 Layer E — Radial growth (pipe model and allometry)

Responsibility: every basal and distal radius in the tree.

Basis: pipe model theory ([Shinozaki et al. 1964, reviewed by Lehnebach et al.,
*The pipe model theory half a century on: a review*, Annals of Botany
2018](https://academic.oup.com/aob/article/121/5/773/4820933)), which
interprets the observed proportionality between conducting stem cross-section
and the leaf mass it supports. We combine it with the Leonardo/da Vinci
branch-diameter rule in generalised exponent form,

    r_parent^Δ = Σ r_child^Δ

with Δ a profile parameter. The classical Leonardo exponent is Δ = 2
([discussion in arXiv:1105.2591](http://arxiv.org/pdf/1105.2591v2.pdf)); measured
values in real trees are commonly above 2. Our profiles use Δ = 2.30 (conifer)
and Δ = 2.49 (broadleaf) as defaults, with the reviewed caveat that the pipe
relationship is not strictly invariant with tree size — conduit diameters widen
from tip toward base, so a single exponent is a first-order description
([Sumida et al., *Allometric scaling of leaf mass based on the pipe model
theory*, Eur. J. Forest Res.
2022](https://link.springer.com/10.1007/s10342-022-01455-7)).

**Approximation and consequence.** A single per-profile Δ plus an age-dependent
basal-flare term. Consequence: taper is plausible and monotone, but we do not
reproduce species-specific tip-to-base conduit widening. Documented in
`limitations.md`.

Additionally required by the directive and enforced as a hard invariant: no
parent radius may fall below any child radius except where an explicit damage,
graft, or profile flag records the deformity.

### 2.6 Layer F — Static mechanics

Responsibility: the final, frozen posture of every axis.

Method: one-time quasi-static solve. Each axis is treated as a tapered
cantilever of profile-specified green-wood modulus, loaded by its own distributed
weight plus the accumulated distal branch and foliage mass. Deflection is
integrated segment-by-segment from base to tip (Euler–Bernoulli small-deflection
per segment, accumulated as finite rotations so total deflection may be large).
A profile-controlled fraction of the deflected posture is then *retained* as
growth curvature, and reaction-wood eccentricity is applied where the bending
moment is largest.

This reproduces the compound profile that real branches show: outward emergence,
mid-span sag, distal re-ascent, because the distal segments were laid down after
the proximal ones had already sagged and their gravitropic set-point is applied
in the deflected frame.

**Approximation.** Static, not dynamic. No creep model, no viscoelasticity, no
wind oscillation. §19 of the directive forbids any post-finalisation motion, so
this is the intended scope, not a shortfall.

### 2.7 Layer G — Branch mortality and self-pruning

Shade-driven, not random. A shoot dies when its accumulated light flux stays
below a profile threshold for a profile-defined number of steps. Dead axes
progress through a recorded state sequence (`ALIVE → SUPPRESSED → DEAD_WITH_BARK
→ DEAD_STUB → OCCLUDED_SCAR`), which is what produces clustered lower-crown
deadwood and knots instead of uniformly healthy branching.

---

## 3. Junction geometry: the single most important realism decision

A branch union is grown tissue, not two intersecting cylinders. The externally
visible features we must generate are the **branch collar** (a swollen shouldered
transition at the branch base) and the **branch bark ridge** (a raised, often
diagonal bark crest above the union). Both are standard arboricultural
landmarks; the ridge of bark in the fork is named the branch bark ridge, and
these structures are central to how a tree responds to branch loss
([Fogliati, *Wounding and Decay in Trees, Part Two*, Magnolia Society
journal](https://magnoliasociety.org/resources/Journal/1986-2011_ISSUES_41-90/ISSUE%2070_03-12_WOUNDING%20AND%20DECAY%20IN%20TREES%20PART%20TWO%20_FRANK%20FOGLIATI.pdf)).

Construction method chosen: **explicit junction topology from a local implicit
field.** Around each union we evaluate a smooth-union scalar field over the
participating tapered generalised cylinders, extract a manifold surface patch,
and stitch it to the parent and child cross-section rings with matching ring
topology. The implicit field is a construction device only; the committed
result is explicit indexed triangles available to the inspection system, as
§4.1/§10.2 of the directive require.

Rejected alternatives and why:
- Boolean CSG on triangle meshes: fragile, produces near-degenerate slivers at
  grazing unions, and destroys the ring correspondence needed for bark flow.
- Pushing one cylinder through another: explicitly forbidden, and it is the
  primary cause of the "plumbing" appearance.

Bark ridge and collar are *not* added as decoration. The collar is produced by
adding a directional radial swelling to the parent cross-sections within an
influence window whose length scales with child radius; the ridge is produced by
constraining the bark displacement field to a compressed, elevated seam along the
union's bisector plane.

---

## 4. Wound response: compartmentalisation, not healing

A tree does not regenerate the tissue it lost. It walls the damaged region off
chemically and physically and grows new tissue *around* it. This is Shigo's
compartmentalisation model, CODIT ([Shigo, *Compartmentalization: a conceptual
framework for understanding how trees grow and defend themselves*, Annual Review
of Phytopathology 1984 —
PDF](https://mapleresearch.org/wp-content/uploads/shigo1984.pdf);
[Penn State Extension, *Understanding the Spread of Decay in
Trees*](https://extension.psu.edu/understanding-the-spread-of-decay-in-trees);
[review of xylem parenchyma in the CODIT context, Front. Plant Sci.
2016](https://www.frontiersin.org/journals/plant-science/articles/10.3389/fpls.2016.01665/full)).

Geometric consequences that the damage system must produce, and which are
therefore acceptance criteria rather than optional polish:

1. The wound cavity persists. Exposed dead wood is recessed below the bark plane.
2. Woundwood advances as **rolled lips** from the margins, thicker laterally than
   vertically, so incomplete closure reads as an eye-shaped feature.
3. Trunk radius is *locally larger* at an old wound than immediately above or
   below it.
4. Bark ridge flow diverts around the wound instead of passing through it.
5. Woundwood grows around, never through, the wound.

---

## 5. Leaf model

### 5.1 Blade and margin

The broadleaf blade is generated as a closed planar outline (lobes, sinuses,
apex, base) that is then given thickness, curvature, and vein-driven relief. It
is never a flat ellipse and never an alpha-masked rectangle.

### 5.2 Venation

Venation is generated as an explicit geometric network, using the same
space-colonization family the tree skeleton uses — which is where the algorithm
originated. The venation model simulates veins growing toward auxin sources
distributed in the blade, sources being suppressed by vein proximity, and both
being reshaped by blade growth ([Runions, Fuhrer, Lane, Federl, Rolland-Lagan &
Prusinkiewicz, *Modeling and visualization of leaf venation patterns*, SIGGRAPH
2005](https://dl.acm.org/doi/10.1145/1073204.1073251)).

Using one algorithm family for both crown and venation is deliberate and
documented: it is the historical relationship between the two models, not code
reuse for convenience. The parameters differ entirely (2D bounded growing domain
with source suppression, versus 3D envelope with light-weighted competition).

The vein network then drives real geometry: midrib and primary veins raise the
lower surface and depress the upper surface, locally thicken the blade, and
control the blade's cross-vein sag between veins. This is required both for
correct micro-shadowing and for the transmission model (§6).

### 5.3 Optical behaviour (informs material parameters, not geometry)

Leaf appearance is the combined result of surface reflection at the cuticle,
internal scattering at cell/air interfaces, pigment absorption, and
transmission; anatomy, water content, pigment concentration and age all modulate
it. Simple diffuse-transmission assumptions are known to be inadequate for
layered thin translucent objects such as leaves. Our leaf material therefore
carries separate cuticular specular, subsurface albedo, and thickness-dependent
transmission terms, and transmission is evaluated against the *geometric*
thickness the mesh actually has — thicker over veins, thinner at the margin.
Overlap darkening is a consequence of real geometry and depth, not an artistic
multiplier.

Because leaves are thin solids and not transparency cards, they are rendered
opaque with a two-sided BRDF plus a transmission lobe; alpha blending is never
enabled for foliage.

---

## 6. Bark model

Bark is subdivided surface geometry. The displacement field is defined in the
axis's **local growth frame** (arc length along the axis, angle around it), never
in world space, so that fissures run with the grain, diverge around unions, and
compress where growth compressed them.

Per-profile bark families implemented in the first release:

- Broadleaf: `SMOOTH` (juvenile twig) → `SHALLOW_FISSURE` (young stem) →
  `DEEP_FURROW_INTERLACING_RIDGE` + `BLOCKY_PLATE` (mature trunk).
- Conifer: `SMOOTH_RESINOUS` → `SCALY` → `THICK_PLATE` with lifted plate edges.

Selection is a continuous function of local radius, cambial age, height, and
exposure — not a random draw — so a single trunk legitimately shows smooth young
branch bark, mature furrowed trunk bark, and polished woundwood simultaneously.

Peeling/lifted plates are generated as shells with real thickness, a raised free
edge, and separation from the inner surface. Transparent planes are prohibited.

---

## 7. Frames, conventions, and numerical decisions

### 7.1 Cross-section frames

Frenet frames are unusable here: they are undefined at zero curvature and flip
at inflection points, both of which occur constantly on tree axes. We use a
**rotation-minimising frame** computed with the *double reflection* method, which
obtains each frame from its predecessor using two reflections, is
fourth-order accurate, and is stable ([Wang, Jüttler, Zheng & Liu, *Computation
of rotation minimizing frames*, ACM TOG 2008 —
PDF](https://ag.jku.at/pubs/2007wjzl.pdf)). This guarantees cross-sections do
not spin along an axis, which is a prerequisite for coherent longitudinal bark
grain.

### 7.2 Binding conventions

| Convention | Value |
|---|---|
| World units | metres |
| Handedness | right-handed world, Y up, −Z forward |
| Matrix storage | column-major, column-vector convention, `M * v` |
| Matrix composition | `P * V * M` |
| Quaternion | `(x, y, z, w)`, unit, right-handed, `q * p * q⁻¹` rotates |
| Angles | radians internally; degrees only at UI boundaries |
| Clip-space depth | reverse-Z, `[1, 0]`, `D32_FLOAT`, `GREATER_EQUAL` |
| Winding | counter-clockwise front face when viewed from outside |
| Serialisation | little-endian, explicitly byte-packed, versioned |
| Alignment | 16-byte for GPU-visible structs; `static_assert` on every one |
| Growth step | one step = one shoot flush; year = profile flushes per year |

### 7.3 Determinism scope (stated precisely, per directive §4)

Guaranteed: **bitwise-identical** organ graph, vertex positions, indices, and
material data for identical `(profile, age, environment, damage, quality, seed)`
on the **same build, same compiler, same optimisation level, same CPU
architecture**.

Not guaranteed: bitwise equality across compilers, across `/O2` vs `/Od`, or
across x64 and ARM64. Reason: permitted floating-point contraction and library
transcendental differences.

Mechanisms: fixed integer iteration order everywhere; no reliance on hash-map
traversal order; per-organ RNG substreams derived from `(root_seed, organ_id,
purpose_tag)` so parallel evaluation cannot change results; no accumulation into
shared floats; `/fp:precise` and no fast-math.

---

## 8. What this document deliberately does not claim

- Not simulated: cell division, water potential, hydraulic conductance, carbon
  allocation chemistry, mycorrhizal interaction, phenology beyond a seasonal
  switch, wind dynamics, epiphyte colonisation.
- Not claimed: global illumination in the raster renderer; species-level
  botanical accuracy for any named species; validated agreement with field
  allometry data.
- The reference forms are *habits*, not taxonomic identifications. The broadleaf
  profile is oak-*like*; it is not *Quercus robur*.
