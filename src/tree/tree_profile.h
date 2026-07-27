/* tree_profile.h -- botanical profiles and correlated parameter resolution.
 *
 * Layer 2. Portable C17.
 *
 * TWO DISTINCT THINGS LIVE HERE, AND THE SEPARATION MATTERS
 *
 *   TreeProfile   A species-level habit: the fixed, age-independent identity of
 *                 a kind of tree. Which architectural model, which phyllotaxis,
 *                 which bark family, which reaction wood, which root plan.
 *                 Data only. Adding a profile must never require engine changes.
 *
 *   TreeResolved  The parameters for ONE individual, produced by combining a
 *                 profile with age, environment, health and seed. This is where
 *                 the project's correlated-randomisation rule is implemented:
 *                 values are derived from each other, never drawn independently.
 *
 * WHY CORRELATION IS ENFORCED HERE AND NOT AT THE DRAW SITES
 *   If every consumer sampled its own random value, an old tree could end up with
 *   juvenile bark, or a wind-exposed tree could be tall and slender. Resolving
 *   once, centrally, makes those combinations impossible to express rather than
 *   merely discouraged. The forbidden combinations listed in the project
 *   directive are prevented structurally: bark family comes from the profile and
 *   its maturity from local radius and age, leaf kind comes from the profile, and
 *   a conifer profile has no way to select broadleaf phyllotaxis.
 *
 * Every field in TreeResolved is consumed by the growth, mechanics or meshing
 * code. There are no unused parameters held for a future that may not arrive.
 */
#ifndef TG_TREE_PROFILE_H
#define TG_TREE_PROFILE_H

#include "../core/math3d.h"
#include "../core/rng.h"

/* --------------------------------------------------------------------------
 * Category and architecture
 * -------------------------------------------------------------------------- */
typedef enum TreeCategory {
    TREE_CATEGORY_BROADLEAF = 0,
    TREE_CATEGORY_CONIFER,
    TREE_CATEGORY_COUNT
} TreeCategory;

/* Hallé-Oldeman-Tomlinson style architectural classes. Only the two the engine
 * actually implements are listed; naming them makes the profile's developmental
 * rules explicit rather than implied by scattered constants. */
typedef enum TreeArchitecture {
    /* Monopodial orthotropic trunk, rhythmic growth, laterals equivalent to the
     * trunk in structure. Apical control weakens with age, giving a decurrent,
     * repeatedly forking crown. Oak-like. */
    TREE_ARCH_RAUH = 0,
    /* Monopodial orthotropic trunk with strong, persistent apical control and
     * plagiotropic branch tiers in pseudo-whorls. Excurrent throughout.
     * Fir/spruce-like. */
    TREE_ARCH_MASSART,
    TREE_ARCH_COUNT
} TreeArchitecture;

typedef enum Phyllotaxis {
    /* Divergence angle 137.5 degrees: the overwhelmingly common case. */
    PHYLLO_SPIRAL_ALTERNATE = 0,
    /* Pairs at 90 degrees to the previous pair. */
    PHYLLO_OPPOSITE_DECUSSATE,
    /* n leaves per node. */
    PHYLLO_WHORLED,
    TREE_PHYLLO_COUNT
} Phyllotaxis;

typedef enum LeafKind {
    LEAF_SIMPLE_LOBED = 0,   /* oak-like: rounded lobes, obtuse sinuses      */
    LEAF_SIMPLE_SERRATE,     /* toothed margin, no lobes                     */
    LEAF_PINNATE_COMPOUND,   /* leaflets on a rachis: ONE leaf organ         */
    LEAF_NEEDLE_SINGLE,      /* fir/spruce: needles inserted individually    */
    LEAF_NEEDLE_FASCICLE,    /* pine: n needles in a sheathed bundle         */
    LEAF_SCALE,              /* cypress-like appressed scales                */
    LEAF_KIND_COUNT
} LeafKind;

/* Coherent bark architectures. A profile names the family it belongs to; the
 * expression of that family at a given point on the tree is a function of local
 * radius, cambial age, height and exposure -- never a random draw. That is what
 * lets one trunk legitimately show smooth young branch bark and deeply furrowed
 * mature trunk bark at the same time without mixing families. */
typedef enum BarkFamily {
    BARK_SMOOTH = 0,
    BARK_SMOOTH_LENTICELLED,
    BARK_SHALLOW_FISSURED,
    BARK_DEEP_FURROWED_RIDGED,   /* interlacing vertical ridges: oak-like    */
    BARK_BLOCKY_PLATED,
    BARK_SCALY,
    BARK_THICK_PLATED_RESINOUS,  /* mature fir/pine                          */
    BARK_FIBROUS_STRINGY,
    BARK_EXFOLIATING_PAPERY,
    BARK_FAMILY_COUNT
} BarkFamily;

typedef enum RootArchitecture {
    ROOT_ARCH_TAPROOT = 0,       /* one dominant descending axis             */
    ROOT_ARCH_HEART,             /* several oblique majors, moderate depth   */
    ROOT_ARCH_PLATE,             /* wide shallow laterals, weak taproot     */
    ROOT_ARCH_BUTTRESSED,        /* plate plus above-ground buttress flares  */
    ROOT_ARCH_COUNT
} RootArchitecture;

/* Reaction wood forms on opposite sides in the two groups, and the visible
 * consequence is the SIGN of cross-section eccentricity. Getting this backwards
 * produces a tree whose branches are subtly, systematically wrong. */
typedef enum ReactionWood {
    REACTION_TENSION_UPPER = 0,     /* angiosperms: pulls, thickens above    */
    REACTION_COMPRESSION_LOWER      /* gymnosperms: pushes, thickens below   */
} ReactionWood;

typedef enum TreeSeason {
    SEASON_SUMMER = 0,   /* full foliage                                    */
    SEASON_SPRING,       /* expanding leaves, visible buds                  */
    SEASON_AUTUMN,       /* senescent colour, some leaves shed              */
    SEASON_WINTER,       /* deciduous profiles bare; buds and scars visible */
    TREE_SEASON_COUNT
} TreeSeason;

typedef enum TreeHealth {
    HEALTH_VIGOROUS = 0,
    HEALTH_TYPICAL,
    HEALTH_STRESSED,
    HEALTH_DAMAGED,
    HEALTH_ANCIENT,     /* retrenching crown, hollowing, major limb loss    */
    TREE_HEALTH_COUNT
} TreeHealth;

typedef enum TreeQuality {
    /* Named honestly: these change how much geometry is generated, and the
     * statistics panel reports what was actually produced. */
    QUALITY_DRAFT = 0,      /* fast iteration; visibly coarser              */
    QUALITY_STANDARD,
    QUALITY_HIGH,
    QUALITY_REFERENCE,      /* maximum detail; the acceptance target        */
    TREE_QUALITY_COUNT
} TreeQuality;

const char *tree_category_name(TreeCategory c);
const char *tree_architecture_name(TreeArchitecture a);
const char *tree_bark_family_name(BarkFamily b);
const char *tree_root_architecture_name(RootArchitecture r);
const char *tree_leaf_kind_name(LeafKind k);
const char *tree_season_name(TreeSeason s);
const char *tree_health_name(TreeHealth h);
const char *tree_quality_name(TreeQuality q);

/* --------------------------------------------------------------------------
 * Environment
 *
 * A small, explicit set of conditions with visible consequences. Each field is
 * used: light_direction and light_anisotropy drive crown displacement,
 * wind_* drive height reduction, basal thickening and windward branch loss,
 * canopy_closure drives lower-branch mortality and crown lift.
 * -------------------------------------------------------------------------- */
typedef struct TreeEnvironment {
    V3  light_direction;   /* unit, direction light comes FROM               */
    f32 light_anisotropy;  /* 0 = even sky, 1 = strongly one-sided           */
    V3  wind_direction;    /* unit, prevailing direction wind blows TOWARD   */
    f32 wind_exposure;     /* 0 = sheltered, 1 = fully exposed ridge         */
    f32 canopy_closure;    /* 0 = open grown, 1 = dense forest interior      */
    f32 soil_resistance;   /* 0 = loose sand, 1 = compacted or stony         */
} TreeEnvironment;

TreeEnvironment tree_environment_open_grown(void);
TreeEnvironment tree_environment_forest(void);
TreeEnvironment tree_environment_exposed_ridge(void);

/* --------------------------------------------------------------------------
 * Settings: the complete, reproducible description of one tree
 *
 * The project's determinism contract is exactly: identical Settings implies
 * identical geometry. Nothing outside this struct may influence generation.
 * -------------------------------------------------------------------------- */
typedef struct TreeSettings {
    TreeCategory    category;
    u32             profile_index;  /* which profile within the category      */
    f32             age_years;
    TreeSeason      season;
    TreeHealth      health;
    TreeQuality     quality;
    TreeEnvironment environment;
    u64             seed;
} TreeSettings;

TreeSettings tree_settings_default(TreeCategory category);
/* Folds every field into a 64-bit value. Used to label captures and to detect
 * that "the same settings" really were the same. */
u64 tree_settings_hash(const TreeSettings *s);

/* --------------------------------------------------------------------------
 * Profile: species-level habit, age independent
 * -------------------------------------------------------------------------- */
typedef struct TreeProfile {
    const char      *name;
    const char      *habit_note;   /* honest description: "oak-like", not a
                                    * species claim                          */
    TreeCategory     category;
    TreeArchitecture architecture;
    Phyllotaxis      phyllotaxis;
    u32              whorl_count;      /* for PHYLLO_WHORLED / branch tiers  */
    LeafKind          leaf_kind;
    u32              leaflets_per_leaf;/* compound leaves; 1 otherwise       */
    u32              needles_per_fascicle;
    BarkFamily       bark_juvenile;
    BarkFamily       bark_mature;
    RootArchitecture root_architecture;
    ReactionWood     reaction_wood;
    bool             deciduous;

    /* Size and rate. Height follows a saturating curve toward mature_height. */
    f32 mature_height_m;
    f32 maturity_age_years;
    f32 juvenile_height_rate;      /* m/year early, before saturation        */
    f32 crown_width_ratio;         /* mature crown width / height            */
    /* Height fraction where the crown is widest. Broadleaf ~0.5 (rounded),
     * conifer ~0.05 (conical, widest at the base). */
    f32 crown_widest_at;
    f32 crown_base_height_ratio;   /* open-grown live-crown base / height    */

    /* Branching. Values are per branch ORDER where an array is given; index 0 is
     * the trunk. */
    /* Per-order arrays are sized for TEN orders, not six.
     *
     * A real mature broadleaf carries eight or more branch orders and on the order
     * of 100,000 living twig tips. With six slots the deepest achievable order was
     * five, which produced 534 living tips and therefore about 4 m^2 of leaf area
     * on a tree that should carry hundreds -- measured three independent ways
     * (leaf area, attractor consumption, and crown radius against its envelope).
     * The array size was the binding constraint, so it is part of the fix. */
    u32 max_branch_order;
    f32 branch_angle_deg[10];      /* insertion angle from the parent axis   */
    f32 branch_angle_spread_deg;   /* +/- variation                          */
    f32 internode_length_m[10];
    f32 internode_length_spread;   /* fractional                             */
    f32 order_length_ratio[10];    /* child axis length / parent remaining   */
    u32 flushes_per_year;

    /* Apical control: the share of a node's resource kept by the apical bud.
     * 0.5 is neutral; higher favours the leader. Decays with age for Rauh. */
    f32 apical_control;
    f32 apical_control_decay_per_year;
    f32 apical_control_min;

    /* Tropisms. Bounded per-step angular budgets in radians. */
    f32 gravitropism[10];          /* toward the axis set-point              */
    f32 plagiotropic_set_angle_deg[10]; /* 0 = vertical, 90 = horizontal     */
    f32 phototropism;
    f32 max_turn_per_step;

    /* Space competition. */
    f32 attractor_density;         /* points per cubic metre of envelope     */
    f32 influence_radius_m;        /* attractors that can steer a tip        */
    f32 kill_radius_m;             /* attractors consumed on arrival         */

    /* Survival. */
    f32 light_death_threshold;     /* below this accumulated light, a shoot dies */
    u32 suppression_tolerance_steps;
    f32 shade_tolerance;           /* 0 = intolerant, 1 = very tolerant      */

    /* Radial growth. */
    f32 leonardo_exponent;         /* r_parent^d = sum r_child^d             */
    f32 tip_radius_m;              /* radius of a first-year shoot tip       */
    f32 basal_flare_factor;        /* extra radius at ground level           */
    f32 basal_flare_height_ratio;  /* flare extent / trunk radius            */

    /* Mechanics. Green-wood values; documented as representative, not measured
     * for a named species. */
    f32 wood_modulus_pa;           /* effective bending modulus              */
    f32 wood_density_kgm3;
    f32 foliage_density_kgm2;      /* mass per unit supported leaf area      */
    f32 sag_retention;             /* fraction of deflection kept as growth  */
    f32 reaction_eccentricity;     /* 0..0.5 of radius                       */

    /* Cross-section shape. */
    f32 lobing_amplitude;          /* fraction of radius                     */
    u32 lobing_lobes;
    f32 collar_length_factor;      /* collar length / child radius           */
    f32 collar_swell_factor;       /* extra parent radius at the union       */
    f32 bark_ridge_prominence;     /* branch-bark ridge height / child radius */

    /* Roots. */
    u32 major_root_count;
    f32 root_spread_ratio;         /* lateral spread / tree height           */
    f32 root_depth_ratio;
    f32 root_flare_ratio;          /* flare radius / trunk radius            */
    f32 taproot_strength;          /* 0 = none, 1 = dominant                 */
    f32 buttress_strength;         /* 0 = none                               */

    /* Foliage. */
    f32 leaf_length_m;
    f32 leaf_width_ratio;
    f32 leaf_thickness_m;
    f32 leaves_per_metre_of_shoot;
    f32 needle_length_m;
    f32 needle_width_m;
} TreeProfile;

/* Built-in profiles. Small in number and correct, rather than many and wrong. */
u32                tree_profile_count(TreeCategory category);
const TreeProfile *tree_profile_get(TreeCategory category, u32 index);
/* Verifies a profile is internally coherent: no conifer with broadleaf
 * phyllotaxis, no compound needles, monotone order arrays, positive sizes.
 * Returns TG_OK or logs the specific inconsistency. */
TgResult           tree_profile_validate(const TreeProfile *p);

/* --------------------------------------------------------------------------
 * Resolved parameters for one individual
 * -------------------------------------------------------------------------- */
typedef struct TreeResolved {
    const TreeProfile *profile;
    TreeSettings       settings;

    /* Derived scalars, all correlated. */
    f32 age_years;
    f32 maturity;            /* 0 = seedling, 1 = mature, >1 = over-mature   */
    f32 height_m;
    f32 crown_width_m;
    f32 crown_base_height_m;
    f32 crown_widest_height_m;
    f32 trunk_base_radius_m;
    f32 lean_angle_rad;
    V3  lean_direction;
    f32 apical_control;      /* age-adjusted                                 */
    f32 crown_asymmetry;     /* 0..1, driven by light anisotropy and wind     */
    V3  crown_offset_dir;    /* unit, direction the crown is displaced        */
    f32 bark_maturity;       /* 0..1 at the trunk base                        */
    f32 deadwood_fraction;   /* expected share of lower-crown shoots dead     */
    f32 wind_form_factor;    /* 0 = unaffected, 1 = strongly wind-formed      */
    f32 basal_thickening;    /* multiplier on trunk_base_radius from exposure  */
    f32 root_spread_m;
    f32 root_depth_m;
    f32 leaf_scale;          /* multiplier on profile leaf size               */
    f32 foliage_density;     /* multiplier on leaves per metre of shoot       */
    u32 growth_steps;        /* total developmental steps to simulate         */

    /* Quality-driven budgets. Reported in the statistics panel next to the
     * botanically plausible count, so any shortfall is visible rather than
     * silent. */
    u32 cross_section_segments_min;
    u32 cross_section_segments_max;
    u32 max_leaves;
    u32 leaf_triangle_budget;
    f32 bark_feature_size_m;   /* target bark subdivision length              */
    u32 max_organs;
} TreeResolved;

/* Combines profile + settings + seed into the resolved individual. Pure: no
 * global state, no hidden inputs. Called exactly once per generation. */
TgResult tree_profile_resolve(const TreeSettings *settings, TreeResolved *out);

/* Crown envelope radius at a height, in metres. The envelope is a construction
 * aid for attractor placement and is never rendered as part of the tree. */
f32 tree_resolved_envelope_radius(const TreeResolved *r, f32 height_m);

/* Per-order helpers that clamp the order into the profile's array bounds, so a
 * higher branch order than the profile describes degrades smoothly instead of
 * reading out of bounds. */
f32 tree_resolved_branch_angle(const TreeResolved *r, u32 order);
f32 tree_resolved_internode_length(const TreeResolved *r, u32 order);
f32 tree_resolved_gravitropism(const TreeResolved *r, u32 order);
f32 tree_resolved_set_angle(const TreeResolved *r, u32 order);
f32 tree_resolved_order_length_ratio(const TreeResolved *r, u32 order);

#endif /* TG_TREE_PROFILE_H */
