#include "tree_profile.h"

#include "../core/hash.h"
#include "../core/log.h"

#include <string.h>

#define TP_SUB "tree_profile"

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

const char *tree_category_name(TreeCategory c) {
    switch (c) {
    case TREE_CATEGORY_BROADLEAF: return "broadleaf";
    case TREE_CATEGORY_CONIFER:   return "conifer";
    case TREE_CATEGORY_COUNT:     break;
    }
    return "invalid";
}

const char *tree_architecture_name(TreeArchitecture a) {
    switch (a) {
    case TREE_ARCH_RAUH:    return "Rauh (monopodial, decurrent with age)";
    case TREE_ARCH_MASSART: return "Massart (monopodial, excurrent, whorled tiers)";
    case TREE_ARCH_COUNT:   break;
    }
    return "invalid";
}

const char *tree_bark_family_name(BarkFamily b) {
    switch (b) {
    case BARK_SMOOTH:               return "smooth";
    case BARK_SMOOTH_LENTICELLED:   return "smooth_lenticelled";
    case BARK_SHALLOW_FISSURED:     return "shallow_fissured";
    case BARK_DEEP_FURROWED_RIDGED: return "deep_furrowed_ridged";
    case BARK_BLOCKY_PLATED:        return "blocky_plated";
    case BARK_SCALY:                return "scaly";
    case BARK_THICK_PLATED_RESINOUS:return "thick_plated_resinous";
    case BARK_FIBROUS_STRINGY:      return "fibrous_stringy";
    case BARK_EXFOLIATING_PAPERY:   return "exfoliating_papery";
    case BARK_FAMILY_COUNT:         break;
    }
    return "invalid";
}

const char *tree_root_architecture_name(RootArchitecture r) {
    switch (r) {
    case ROOT_ARCH_TAPROOT:     return "taproot";
    case ROOT_ARCH_HEART:       return "heart";
    case ROOT_ARCH_PLATE:       return "plate";
    case ROOT_ARCH_BUTTRESSED:  return "buttressed";
    case ROOT_ARCH_COUNT:       break;
    }
    return "invalid";
}

const char *tree_leaf_kind_name(LeafKind k) {
    switch (k) {
    case LEAF_SIMPLE_LOBED:      return "simple_lobed";
    case LEAF_SIMPLE_SERRATE:    return "simple_serrate";
    case LEAF_PINNATE_COMPOUND:  return "pinnate_compound";
    case LEAF_NEEDLE_SINGLE:     return "needle_single";
    case LEAF_NEEDLE_FASCICLE:   return "needle_fascicle";
    case LEAF_SCALE:             return "scale";
    case LEAF_KIND_COUNT:        break;
    }
    return "invalid";
}

const char *tree_season_name(TreeSeason s) {
    switch (s) {
    case SEASON_SUMMER: return "summer";
    case SEASON_SPRING: return "spring";
    case SEASON_AUTUMN: return "autumn";
    case SEASON_WINTER: return "winter";
    case TREE_SEASON_COUNT: break;
    }
    return "invalid";
}

const char *tree_health_name(TreeHealth h) {
    switch (h) {
    case HEALTH_VIGOROUS: return "vigorous";
    case HEALTH_TYPICAL:  return "typical";
    case HEALTH_STRESSED: return "stressed";
    case HEALTH_DAMAGED:  return "damaged";
    case HEALTH_ANCIENT:  return "ancient";
    case TREE_HEALTH_COUNT: break;
    }
    return "invalid";
}

const char *tree_quality_name(TreeQuality q) {
    switch (q) {
    case QUALITY_DRAFT:     return "draft";
    case QUALITY_STANDARD:  return "standard";
    case QUALITY_HIGH:      return "high";
    case QUALITY_REFERENCE: return "reference";
    case TREE_QUALITY_COUNT: break;
    }
    return "invalid";
}

/* ------------------------------------------------------------------------- */
/* Environments                                                              */
/* ------------------------------------------------------------------------- */

TreeEnvironment tree_environment_open_grown(void) {
    TreeEnvironment e;
    /* Light predominantly from above with a mild southerly bias, which is why
     * open-grown trees are broad and only slightly asymmetric. */
    e.light_direction = v3_norm_or(v3(0.15f, 1.0f, 0.25f), v3(0, 1, 0));
    e.light_anisotropy = 0.15f;
    e.wind_direction = v3_norm_or(v3(1.0f, 0.0f, 0.3f), v3(1, 0, 0));
    e.wind_exposure = 0.2f;
    e.canopy_closure = 0.0f;
    e.soil_resistance = 0.35f;
    return e;
}

TreeEnvironment tree_environment_forest(void) {
    TreeEnvironment e;
    e.light_direction = v3_norm_or(v3(0.05f, 1.0f, 0.05f), v3(0, 1, 0));
    e.light_anisotropy = 0.1f;
    e.wind_direction = v3_norm_or(v3(1.0f, 0.0f, 0.0f), v3(1, 0, 0));
    e.wind_exposure = 0.05f;
    /* The single most consequential environmental value: it lifts the crown and
     * kills lower branches, which is what makes a forest tree tall and clean
     * where an open-grown one of the same profile is broad and low-branched. */
    e.canopy_closure = 0.85f;
    e.soil_resistance = 0.4f;
    return e;
}

TreeEnvironment tree_environment_exposed_ridge(void) {
    TreeEnvironment e;
    e.light_direction = v3_norm_or(v3(0.4f, 1.0f, 0.0f), v3(0, 1, 0));
    e.light_anisotropy = 0.35f;
    e.wind_direction = v3_norm_or(v3(1.0f, 0.0f, 0.0f), v3(1, 0, 0));
    e.wind_exposure = 0.95f;
    e.canopy_closure = 0.0f;
    e.soil_resistance = 0.8f;
    return e;
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

TreeSettings tree_settings_default(TreeCategory category) {
    TreeSettings s;
    memset(&s, 0, sizeof s);
    s.category = (u32)category < (u32)TREE_CATEGORY_COUNT
                   ? category : TREE_CATEGORY_BROADLEAF;
    s.profile_index = 0;
    s.age_years = 80.0f;
    s.season = SEASON_SUMMER;
    s.health = HEALTH_TYPICAL;
    s.quality = QUALITY_STANDARD;
    s.environment = tree_environment_open_grown();
    s.seed = 0x5EEDC0FFEEull;
    return s;
}

u64 tree_settings_hash(const TreeSettings *s) {
    u64 h = TG_FNV64_OFFSET;
    TG_CHECK(s != NULL);
    h = tg_hash64_u32(h, (u32)s->category);
    h = tg_hash64_u32(h, s->profile_index);
    h = tg_hash64_bytes(h, &s->age_years, sizeof s->age_years);
    h = tg_hash64_u32(h, (u32)s->season);
    h = tg_hash64_u32(h, (u32)s->health);
    h = tg_hash64_u32(h, (u32)s->quality);
    h = tg_hash64_bytes(h, &s->environment, sizeof s->environment);
    h = tg_hash64_u64(h, s->seed);
    return h;
}

/* ------------------------------------------------------------------------- */
/* Built-in profiles                                                         */
/* ------------------------------------------------------------------------- */

/* Broadleaf reference habit: open-grown temperate deciduous, oak-LIKE.
 *
 * This is a habit, not a species identification. Values are chosen to be
 * mutually consistent and to sit inside the ranges reported for large temperate
 * white-oak-group trees; they are not measured field allometry, and
 * docs/limitations.md says so. */
static const TreeProfile g_broadleaf[] = {
    {
        "Broadleaf (oak-like)",
        "Open-grown temperate deciduous habit. Monopodial when young, becoming "
        "decurrent with repeated forking as apical control decays. Not a species "
        "identification.",
        TREE_CATEGORY_BROADLEAF,
        TREE_ARCH_RAUH,
        PHYLLO_SPIRAL_ALTERNATE,
        0,                              /* whorl_count: not whorled          */
        LEAF_SIMPLE_LOBED,
        1,                              /* leaflets_per_leaf: simple leaf    */
        0,                              /* needles_per_fascicle: n/a         */
        BARK_SMOOTH,
        BARK_DEEP_FURROWED_RIDGED,
        ROOT_ARCH_HEART,
        REACTION_TENSION_UPPER,         /* angiosperm                        */
        true,                           /* deciduous                         */

        26.0f,                          /* mature_height_m                   */
        140.0f,                         /* maturity_age_years                */
        0.43f,                          /* juvenile_height_rate m/yr         */
        1.15f,                          /* crown_width_ratio: wider than tall */
        0.52f,                          /* crown_widest_at: rounded crown    */
        0.28f,                          /* crown_base_height_ratio           */

        5,                              /* max_branch_order                  */
        { 0.0f, 48.0f, 55.0f, 62.0f, 68.0f, 72.0f },
        14.0f,                          /* branch_angle_spread_deg           */
        /* Order 0 IS the juvenile annual height increment: the growth model
         * decays it as exp(-t/k), so summing the leader's internodes over the
         * whole history reproduces mature_height exactly. Enforced by
         * tree_profile_validate. */
        { 0.43f, 0.34f, 0.20f, 0.12f, 0.070f, 0.045f },
        0.30f,                          /* internode_length_spread           */
        { 1.0f, 0.62f, 0.55f, 0.48f, 0.40f, 0.32f },
        1,                              /* flushes_per_year                  */

        0.62f,                          /* apical_control                    */
        0.0022f,                        /* decay per year -> decurrent crown */
        0.42f,                          /* apical_control_min                */

        /* Gravitropism: strong on the trunk, weak on high-order twigs, which is
         * what lets fine shoots wander while the trunk stays upright. */
        { 0.85f, 0.30f, 0.20f, 0.14f, 0.10f, 0.08f },
        /* Set angles: the trunk is vertical, primaries ascend, higher orders sit
         * progressively closer to horizontal. */
        { 0.0f, 42.0f, 55.0f, 66.0f, 74.0f, 80.0f },
        0.45f,                          /* phototropism                      */
        0.42f,                          /* max_turn_per_step (rad)           */

        /* Density is CALIBRATED against the influence radius, not chosen for
         * looks: the influence sphere has volume (4/3)pi*1.6^3 = 17.2 m^3, so
         * 2.5 points per m^3 puts about 43 attractors in range of a tip. Enough
         * for a well-conditioned average direction, and far below the query
         * buffer so the result is never truncated. */
        2.5f,                           /* attractor_density per m^3         */
        1.6f,                           /* influence_radius_m                */
        0.42f,                          /* kill_radius_m                     */

        0.16f,                          /* light_death_threshold             */
        4,                              /* suppression_tolerance_steps       */
        0.45f,                          /* shade_tolerance                   */

        2.49f,                          /* leonardo_exponent (see research)  */
        0.0022f,                        /* tip_radius_m                      */
        0.55f,                          /* basal_flare_factor                */
        2.6f,                           /* basal_flare_height_ratio          */

        7.5e9f,                         /* wood_modulus_pa (green hardwood)  */
        760.0f,                         /* wood_density_kgm3                 */
        0.115f,                         /* foliage_density_kgm2              */
        0.72f,                          /* sag_retention                     */
        0.16f,                          /* reaction_eccentricity             */

        0.055f,                         /* lobing_amplitude                  */
        7,                              /* lobing_lobes                      */
        1.9f,                           /* collar_length_factor              */
        0.34f,                          /* collar_swell_factor               */
        0.42f,                          /* bark_ridge_prominence             */

        6,                              /* major_root_count                  */
        /* Root spread is a multiple of CROWN WIDTH, not of height. Tying it to
         * height gave a 21 m root radius around a 5 m crown -- four times the
         * crown radius, which no temperate tree does. Field guidance puts the
         * structural root plate at roughly one to three times the crown radius. */
        1.20f,                          /* root_spread_ratio (x crown width) */
        0.16f,                          /* root_depth_ratio                  */
        1.85f,                          /* root_flare_ratio                  */
        0.35f,                          /* taproot_strength                  */
        0.0f,                           /* buttress_strength                 */

        0.115f,                         /* leaf_length_m                     */
        0.62f,                          /* leaf_width_ratio                  */
        0.00022f,                       /* leaf_thickness_m                  */
        28.0f,                          /* leaves_per_metre_of_shoot         */
        0.0f, 0.0f                      /* needle dimensions: n/a            */
    }
};

/* Conifer reference habit: temperate evergreen, fir/spruce-LIKE. */
static const TreeProfile g_conifer[] = {
    {
        "Conifer (fir-like)",
        "Temperate evergreen habit with persistent strong apical control and "
        "pseudo-whorled plagiotropic branch tiers. Not a species identification.",
        TREE_CATEGORY_CONIFER,
        TREE_ARCH_MASSART,
        PHYLLO_SPIRAL_ALTERNATE,        /* needles inserted spirally          */
        5,                              /* whorl_count: branches per tier     */
        LEAF_NEEDLE_SINGLE,
        1,
        0,                              /* single needles, no fascicle sheath */
        BARK_SMOOTH,
        BARK_THICK_PLATED_RESINOUS,
        ROOT_ARCH_PLATE,
        REACTION_COMPRESSION_LOWER,     /* gymnosperm                        */
        false,                          /* evergreen                         */

        34.0f,                          /* mature_height_m: taller, narrower  */
        110.0f,
        0.71f,
        0.36f,                          /* crown_width_ratio: narrow cone     */
        0.06f,                          /* crown_widest_at: near the base      */
        0.10f,                          /* crown_base_height_ratio: low skirt  */

        /* Three orders, not four. A conifer whorl already contributes five
         * laterals per annual node on the trunk, so a fourth order multiplied the
         * shoot count to twelve times the broadleaf's per unit crown volume and
         * 86% of all shoot segments died of self-shading -- a tree made mostly of
         * deadwood. Reducing the cascade is the correct fix: the crowding was the
         * cause, the mortality only the symptom. */
        3,
        { 0.0f, 82.0f, 74.0f, 70.0f, 68.0f, 66.0f },
        9.0f,                           /* tighter: whorls are regular        */
        { 0.71f, 0.30f, 0.17f, 0.090f, 0.055f, 0.040f },
        0.18f,
        { 1.0f, 0.50f, 0.46f, 0.40f, 0.34f, 0.30f },
        1,

        0.86f,                          /* strong, persistent apical control  */
        0.0004f,                        /* barely decays: stays excurrent     */
        0.78f,

        { 0.95f, 0.16f, 0.12f, 0.10f, 0.08f, 0.07f },
        /* Near-horizontal tiers, with the outer orders drooping slightly. */
        { 0.0f, 86.0f, 92.0f, 96.0f, 98.0f, 100.0f },
        0.22f,                          /* less phototropic than broadleaf    */
        0.30f,

        /* Influence sphere (4/3)pi*1.1^3 = 5.6 m^3; 5 per m^3 gives ~28 in
         * range. Denser than the broadleaf because conifer internodes and the
         * kill radius are both smaller. */
        5.0f,                           /* attractor_density per m^3         */
        1.1f,                           /* influence_radius_m                */
        0.30f,                          /* kill_radius_m                     */

        /* Conifer crowns are far denser than broadleaf crowns by construction
         * (five laterals per annual whorl), so their shoots must tolerate more
         * self-shading or the model kills nearly all of them: measured 88.7% of
         * shoot segments dead before this was corrected. Evergreen conifers do
         * hold suppressed branches for many years, so a longer tolerance is
         * biologically right as well as necessary. */
        0.07f,                          /* light_death_threshold             */
        7,                              /* suppression_tolerance_steps       */
        0.55f,                          /* shade_tolerance                   */

        2.30f,                          /* leonardo_exponent                 */
        0.0016f,
        0.40f,
        2.2f,

        9.0e9f,                         /* stiffer relative to its loading    */
        520.0f,
        0.32f,                          /* evergreen: much more supported mass */
        0.55f,
        0.20f,                          /* compression wood is pronounced     */

        0.030f,
        9,
        1.5f,
        0.26f,
        0.30f,

        5,
        1.80f,                          /* root_spread_ratio: wide shallow plate */
        0.09f,
        1.55f,
        0.10f,                          /* weak taproot in the mature form    */
        0.0f,

        0.0f, 0.0f, 0.0f, 0.0f,         /* broadleaf leaf fields unused       */
        0.024f,                         /* needle_length_m                   */
        0.0016f                         /* needle_width_m                    */
    }
};

u32 tree_profile_count(TreeCategory category) {
    switch (category) {
    case TREE_CATEGORY_BROADLEAF: return (u32)TG_COUNTOF(g_broadleaf);
    case TREE_CATEGORY_CONIFER:   return (u32)TG_COUNTOF(g_conifer);
    case TREE_CATEGORY_COUNT:     break;
    }
    return 0;
}

const TreeProfile *tree_profile_get(TreeCategory category, u32 index) {
    switch (category) {
    case TREE_CATEGORY_BROADLEAF:
        return index < TG_COUNTOF(g_broadleaf) ? &g_broadleaf[index] : NULL;
    case TREE_CATEGORY_CONIFER:
        return index < TG_COUNTOF(g_conifer) ? &g_conifer[index] : NULL;
    case TREE_CATEGORY_COUNT:
        break;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Profile coherence                                                         */
/*                                                                           */
/* This is where the directive's forbidden combinations are made impossible    */
/* rather than merely discouraged. A profile that mixes incompatible botany     */
/* fails here and never reaches the generator.                                 */
/* ------------------------------------------------------------------------- */

TgResult tree_profile_validate(const TreeProfile *p) {
    u32 i;
    bool ok = true;

    if (p == NULL) { return TG_ERR_INVALID_ARGUMENT; }

#define TP_REQUIRE(cond, ...)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            TG_LOG_ERRORF(TP_SUB, "profile '%s': " __VA_ARGS__,               \
                          p->name != NULL ? p->name : "?");                   \
            ok = false;                                                       \
        }                                                                     \
    } while (0)

    TP_REQUIRE(p->name != NULL && p->habit_note != NULL, "missing name or note");
    TP_REQUIRE((u32)p->category < (u32)TREE_CATEGORY_COUNT, "bad category");
    TP_REQUIRE((u32)p->architecture < (u32)TREE_ARCH_COUNT, "bad architecture");
    TP_REQUIRE((u32)p->leaf_kind < (u32)LEAF_KIND_COUNT, "bad leaf kind");

    /* Category / foliage coherence. A conifer with broad leaves, or a broadleaf
     * with needles, is one of the explicitly forbidden combinations. */
    if (p->category == TREE_CATEGORY_CONIFER) {
        TP_REQUIRE(p->leaf_kind == LEAF_NEEDLE_SINGLE ||
                   p->leaf_kind == LEAF_NEEDLE_FASCICLE ||
                   p->leaf_kind == LEAF_SCALE,
                   "conifer must have needle or scale foliage");
        TP_REQUIRE(p->reaction_wood == REACTION_COMPRESSION_LOWER,
                   "gymnosperm reaction wood must be compression wood (lower side)");
        TP_REQUIRE(p->needle_length_m > 0.0f && p->needle_width_m > 0.0f,
                   "conifer needs needle dimensions");
    } else {
        TP_REQUIRE(p->leaf_kind == LEAF_SIMPLE_LOBED ||
                   p->leaf_kind == LEAF_SIMPLE_SERRATE ||
                   p->leaf_kind == LEAF_PINNATE_COMPOUND,
                   "broadleaf must have broad foliage");
        TP_REQUIRE(p->reaction_wood == REACTION_TENSION_UPPER,
                   "angiosperm reaction wood must be tension wood (upper side)");
        TP_REQUIRE(p->leaf_length_m > 0.0f && p->leaf_width_ratio > 0.0f,
                   "broadleaf needs leaf dimensions");
    }

    /* Fascicles only make sense for the fascicle leaf kind, and a fascicle needs
     * at least two needles or it is not a bundle. */
    if (p->leaf_kind == LEAF_NEEDLE_FASCICLE) {
        TP_REQUIRE(p->needles_per_fascicle >= 2,
                   "fascicle foliage needs at least 2 needles per bundle");
    } else {
        TP_REQUIRE(p->needles_per_fascicle == 0,
                   "needles_per_fascicle set on non-fascicle foliage");
    }
    /* Leaflets belong to compound leaves only; treating leaflets as independent
     * leaves is explicitly forbidden, so the data model must not allow it. */
    if (p->leaf_kind == LEAF_PINNATE_COMPOUND) {
        TP_REQUIRE(p->leaflets_per_leaf >= 3, "compound leaf needs >= 3 leaflets");
    } else if (p->category == TREE_CATEGORY_BROADLEAF) {
        TP_REQUIRE(p->leaflets_per_leaf == 1,
                   "simple leaf must have exactly 1 leaflet");
    }

    if (p->phyllotaxis == PHYLLO_WHORLED) {
        TP_REQUIRE(p->whorl_count >= 3, "whorled phyllotaxis needs >= 3 per node");
    }
    /* Massart architecture is defined by branch tiers, so it needs a tier size. */
    if (p->architecture == TREE_ARCH_MASSART) {
        TP_REQUIRE(p->whorl_count >= 3,
                   "Massart architecture needs a branch tier size >= 3");
    }

    /* Juvenile bark must not already be the mature family: bark maturity is a
     * function of age and radius, and a tree cannot be born furrowed. */
    TP_REQUIRE(p->bark_juvenile != p->bark_mature ||
               p->bark_juvenile == BARK_SMOOTH,
               "juvenile and mature bark families are identical");
    TP_REQUIRE(p->bark_juvenile == BARK_SMOOTH ||
               p->bark_juvenile == BARK_SMOOTH_LENTICELLED ||
               p->bark_juvenile == BARK_EXFOLIATING_PAPERY,
               "juvenile bark must be a smooth or papery family");

    TP_REQUIRE(p->max_branch_order >= 1 &&
               p->max_branch_order < TG_COUNTOF(p->branch_angle_deg),
               "max_branch_order out of the supported range");
    TP_REQUIRE(p->mature_height_m > 0.1f, "non-positive mature height");
    TP_REQUIRE(p->maturity_age_years > 1.0f, "non-positive maturity age");
    TP_REQUIRE(p->crown_width_ratio > 0.0f, "non-positive crown width ratio");
    TP_REQUIRE(p->crown_widest_at >= 0.0f && p->crown_widest_at <= 1.0f,
               "crown_widest_at outside [0,1]");
    TP_REQUIRE(p->crown_base_height_ratio >= 0.0f &&
               p->crown_base_height_ratio < 1.0f,
               "crown_base_height_ratio outside [0,1)");
    TP_REQUIRE(p->leonardo_exponent > 1.5f && p->leonardo_exponent < 4.0f,
               "leonardo exponent outside a physically sensible range");
    /* COHERENCE BETWEEN THE HEIGHT MODEL AND THE GROWTH MODEL.
     *
     * The height curve is h(t) = H(1 - exp(-t/k)) with k = maturity_age/ln(10),
     * so its initial slope is H/k. The growth simulation extends the leader by
     * internode_length_m[0] * exp(-t/k) each flush, which integrates to exactly H
     * only if internode_length_m[0] equals H/k. If the two disagree, the grown
     * tree systematically misses its own target height -- a 60-year tree came out
     * 29.7 m against a 16.1 m target before this was enforced. */
    {
        f32 k = p->maturity_age_years / 2.302585093f;
        f32 expected = p->mature_height_m / tg_maxf(k, 0.1f)
                     / (f32)tg_max_u32(p->flushes_per_year, 1u);
        TP_REQUIRE(tg_absf(p->internode_length_m[0] - expected)
                       < expected * 0.15f,
                   "internode_length_m[0] must equal the juvenile annual height "
                   "increment mature_height/(maturity_age/ln10) within 15%");
        TP_REQUIRE(tg_absf(p->juvenile_height_rate - expected) < expected * 0.20f,
                   "juvenile_height_rate is inconsistent with the height curve");
    }
    TP_REQUIRE(p->tip_radius_m > 0.0f, "non-positive tip radius");
    TP_REQUIRE(p->wood_modulus_pa > 1.0e8f, "implausibly low wood modulus");
    TP_REQUIRE(p->wood_density_kgm3 > 100.0f, "implausibly low wood density");
    TP_REQUIRE(p->sag_retention >= 0.0f && p->sag_retention <= 1.0f,
               "sag_retention outside [0,1]");
    TP_REQUIRE(p->reaction_eccentricity >= 0.0f && p->reaction_eccentricity < 0.5f,
               "reaction_eccentricity must stay below 0.5 of the radius");
    TP_REQUIRE(p->apical_control > 0.0f && p->apical_control < 1.0f,
               "apical_control outside (0,1)");
    TP_REQUIRE(p->apical_control_min > 0.0f &&
               p->apical_control_min <= p->apical_control,
               "apical_control_min must be positive and <= apical_control");
    TP_REQUIRE(p->influence_radius_m > p->kill_radius_m,
               "influence radius must exceed the kill radius");
    TP_REQUIRE(p->attractor_density > 0.0f, "non-positive attractor density");
    TP_REQUIRE(p->major_root_count >= 2, "a tree needs at least 2 major roots");
    TP_REQUIRE(p->root_spread_ratio > 0.0f && p->root_depth_ratio > 0.0f,
               "non-positive root extent");
    TP_REQUIRE(p->root_spread_ratio >= 0.5f && p->root_spread_ratio <= 4.0f,
               "root_spread_ratio is a multiple of CROWN WIDTH and must stay in "
               "[0.5, 4.0]");
    TP_REQUIRE(p->root_flare_ratio > 1.0f,
               "root flare must be wider than the trunk");
    TP_REQUIRE(p->max_turn_per_step > 0.0f && p->max_turn_per_step < TG_PI_F,
               "max_turn_per_step outside (0, pi)");

    /* Per-order arrays must be monotone in the directions biology requires:
     * higher orders are shorter and thinner-supported, and set angles move
     * toward horizontal. A profile violating this produces branch orders that
     * are scaled copies, which the directive forbids. */
    for (i = 1; i <= p->max_branch_order; ++i) {
        TP_REQUIRE(p->internode_length_m[i] > 0.0f,
                   "non-positive internode length at some order");
        TP_REQUIRE(p->internode_length_m[i] < p->internode_length_m[i - 1],
                   "internode length must decrease with branch order");
        TP_REQUIRE(p->order_length_ratio[i] > 0.0f &&
                   p->order_length_ratio[i] < 1.0f,
                   "order_length_ratio must lie in (0,1) for lateral orders");
        TP_REQUIRE(p->plagiotropic_set_angle_deg[i] >=
                       p->plagiotropic_set_angle_deg[i - 1],
                   "set angle must not become more vertical with order");
        TP_REQUIRE(p->gravitropism[i] <= p->gravitropism[i - 1],
                   "gravitropism must not increase with branch order");
    }
    TP_REQUIRE(p->plagiotropic_set_angle_deg[0] == 0.0f,
               "the trunk set angle must be vertical");
    TP_REQUIRE(p->order_length_ratio[0] == 1.0f,
               "the trunk length ratio must be 1");

#undef TP_REQUIRE

    return ok ? TG_OK : TG_ERR_VALIDATION_FAILED;
}

/* ------------------------------------------------------------------------- */
/* Resolution                                                                */
/* ------------------------------------------------------------------------- */

/* Saturating height curve: monotone, zero at age zero, approaching
 * mature_height asymptotically rather than crossing it. Chosen over a linear
 * ramp because a linear model makes an over-mature tree absurdly tall, and over
 * a logistic because real height growth has no inflection after establishment. */
static f32 height_for_age(const TreeProfile *p, f32 age) {
    f32 k;
    if (age <= 0.0f) { return 0.0f; }
    /* h(t) = H * (1 - exp(-t/tau)), with tau chosen so that h(maturity_age) is
     * 90% of H. That fixes the early slope near juvenile_height_rate. */
    k = p->maturity_age_years / 2.302585093f; /* ln(10) => 90% at maturity   */
    if (k < 0.1f) { k = 0.1f; }
    return p->mature_height_m * (1.0f - expf(-age / k));
}

f32 tree_resolved_envelope_radius(const TreeResolved *r, f32 height_m) {
    f32 h, t, peak, radius;

    TG_CHECK(r != NULL);
    h = r->height_m;
    if (!(h > 0.0f)) { return 0.0f; }

    /* Normalised height within the LIVE CROWN, not within the whole tree: below
     * the crown base there is trunk but no crown, and treating the envelope as
     * spanning the full height would place attractors around the bole. */
    if (height_m <= r->crown_base_height_m) { return 0.0f; }
    if (height_m >= h) { return 0.0f; }

    t = (height_m - r->crown_base_height_m) / (h - r->crown_base_height_m);
    t = tg_saturatef(t);

    peak = (r->crown_widest_height_m - r->crown_base_height_m)
         / tg_maxf(h - r->crown_base_height_m, 1e-4f);
    peak = tg_clampf(peak, 0.02f, 0.98f);

    /* Two half-profiles meeting at the widest point, so a conical conifer
     * (peak near 0) and a rounded broadleaf (peak near 0.5) come out of the same
     * expression without a special case. The 0.7 exponent below the peak makes
     * the lower crown fill out convexly; the 1.4 above it tapers concavely
     * toward the leader, which is what gives a conifer its spire. */
    if (t <= peak) {
        radius = powf(t / peak, 0.7f);
    } else {
        radius = powf((1.0f - t) / (1.0f - peak), 1.4f);
    }
    return radius * r->crown_width_m * 0.5f;
}

f32 tree_resolved_branch_angle(const TreeResolved *r, u32 order) {
    u32 i = tg_min_u32(order, (u32)TG_COUNTOF(r->profile->branch_angle_deg) - 1u);
    return r->profile->branch_angle_deg[i] * TG_DEG2RAD_F;
}
f32 tree_resolved_internode_length(const TreeResolved *r, u32 order) {
    u32 i = tg_min_u32(order, (u32)TG_COUNTOF(r->profile->internode_length_m) - 1u);
    /* Internodes scale with the individual's realised size, not with the mature
     * size of the species: a young tree has genuinely shorter shoots. */
    f32 scale = tg_lerpf(0.45f, 1.0f, tg_saturatef(r->maturity));
    return r->profile->internode_length_m[i] * scale;
}
f32 tree_resolved_gravitropism(const TreeResolved *r, u32 order) {
    u32 i = tg_min_u32(order, (u32)TG_COUNTOF(r->profile->gravitropism) - 1u);
    return r->profile->gravitropism[i];
}
f32 tree_resolved_set_angle(const TreeResolved *r, u32 order) {
    u32 i = tg_min_u32(order,
                       (u32)TG_COUNTOF(r->profile->plagiotropic_set_angle_deg) - 1u);
    return r->profile->plagiotropic_set_angle_deg[i] * TG_DEG2RAD_F;
}
f32 tree_resolved_order_length_ratio(const TreeResolved *r, u32 order) {
    u32 i = tg_min_u32(order, (u32)TG_COUNTOF(r->profile->order_length_ratio) - 1u);
    return r->profile->order_length_ratio[i];
}

/* Health modifies vigour, deadwood and asymmetry together. Keeping the mapping
 * in one table makes it impossible for a "vigorous" tree to acquire an ancient
 * tree's deadwood by accident. */
typedef struct HealthFactors {
    f32 deadwood;      /* base share of lower-crown shoots that die         */
    f32 vigor;         /* multiplier on shoot extension                     */
    f32 asymmetry;     /* additional crown imbalance                        */
    f32 leaf_scale;
    f32 foliage;
} HealthFactors;

static HealthFactors health_factors(TreeHealth h) {
    HealthFactors f;
    switch (h) {
    case HEALTH_VIGOROUS: f.deadwood = 0.03f; f.vigor = 1.15f; f.asymmetry = 0.02f;
                          f.leaf_scale = 1.10f; f.foliage = 1.15f; break;
    case HEALTH_TYPICAL:  f.deadwood = 0.10f; f.vigor = 1.00f; f.asymmetry = 0.06f;
                          f.leaf_scale = 1.00f; f.foliage = 1.00f; break;
    case HEALTH_STRESSED: f.deadwood = 0.26f; f.vigor = 0.78f; f.asymmetry = 0.14f;
                          f.leaf_scale = 0.82f; f.foliage = 0.68f; break;
    case HEALTH_DAMAGED:  f.deadwood = 0.34f; f.vigor = 0.85f; f.asymmetry = 0.26f;
                          f.leaf_scale = 0.92f; f.foliage = 0.80f; break;
    case HEALTH_ANCIENT:  f.deadwood = 0.46f; f.vigor = 0.55f; f.asymmetry = 0.22f;
                          f.leaf_scale = 0.86f; f.foliage = 0.62f; break;
    case TREE_HEALTH_COUNT:
    default:              f.deadwood = 0.10f; f.vigor = 1.00f; f.asymmetry = 0.06f;
                          f.leaf_scale = 1.00f; f.foliage = 1.00f; break;
    }
    return f;
}

typedef struct QualityBudget {
    u32 seg_min, seg_max;
    u32 max_leaves;
    u32 leaf_tris;
    f32 bark_feature_m;
    u32 max_organs;
} QualityBudget;

static QualityBudget quality_budget(TreeQuality q) {
    QualityBudget b;
    switch (q) {
    case QUALITY_DRAFT:
        b.seg_min = 8;  b.seg_max = 20;  b.max_leaves = 2000;
        b.leaf_tris = 40;   b.bark_feature_m = 0.060f; b.max_organs = 40000; break;
    case QUALITY_STANDARD:
        b.seg_min = 12; b.seg_max = 40;  b.max_leaves = 20000;
        b.leaf_tris = 140;  b.bark_feature_m = 0.024f; b.max_organs = 200000; break;
    case QUALITY_HIGH:
        b.seg_min = 18; b.seg_max = 72;  b.max_leaves = 70000;
        b.leaf_tris = 340;  b.bark_feature_m = 0.011f; b.max_organs = 600000; break;
    case QUALITY_REFERENCE:
    case TREE_QUALITY_COUNT:
    default:
        b.seg_min = 24; b.seg_max = 128; b.max_leaves = 220000;
        b.leaf_tris = 780;  b.bark_feature_m = 0.005f; b.max_organs = 1500000; break;
    }
    return b;
}

TgResult tree_profile_resolve(const TreeSettings *settings, TreeResolved *out) {
    const TreeProfile *p;
    TgResult r;
    HealthFactors hf;
    QualityBudget qb;
    TgRng rng_form, rng_lean, rng_crown;
    f32 age;
    const TreeEnvironment *env;

    TG_CHECK(out != NULL);
    if (settings == NULL) { return TG_ERR_INVALID_ARGUMENT; }

    memset(out, 0, sizeof *out);

    p = tree_profile_get(settings->category, settings->profile_index);
    if (p == NULL) {
        TG_LOG_ERRORF(TP_SUB, "no profile %u for category %s",
                      settings->profile_index,
                      tree_category_name(settings->category));
        return TG_ERR_NOT_FOUND;
    }
    r = tree_profile_validate(p);
    if (r != TG_OK) { return r; }

    if (!(settings->age_years > 0.0f) || !tg_finitef(settings->age_years)) {
        TG_LOG_ERRORF(TP_SUB, "age must be positive and finite");
        return TG_ERR_INVALID_ARGUMENT;
    }
    env = &settings->environment;
    if (!v3_finite(env->light_direction) || !v3_finite(env->wind_direction)) {
        TG_LOG_ERRORF(TP_SUB, "environment direction is not finite");
        return TG_ERR_INVALID_ARGUMENT;
    }

    out->profile = p;
    out->settings = *settings;
    /* Clamp rather than reject: an absurd age is a UI slip, and the honest
     * response is to build the oldest tree the model supports. */
    age = tg_clampf(settings->age_years, 0.5f, p->maturity_age_years * 6.0f);
    out->age_years = age;
    out->maturity = tg_saturatef(age / p->maturity_age_years);

    hf = health_factors(settings->health);
    qb = quality_budget(settings->quality);

    rng_form  = tg_rng_substream(settings->seed, TG_RNG_TRUNK_FORM, 0, 0);
    rng_lean  = tg_rng_substream(settings->seed, TG_RNG_TRUNK_LEAN, 0, 0);
    rng_crown = tg_rng_substream(settings->seed, TG_RNG_CROWN_ENVELOPE, 0, 0);

    /* --- height ------------------------------------------------------------
     * Correlated, in order: base curve, then wind suppression, then canopy
     * competition, then individual variation. Wind exposure SHORTENS and
     * THICKENS (thigmomorphogenesis); canopy closure lengthens the bole and
     * raises the crown. */
    out->wind_form_factor = tg_saturatef(env->wind_exposure);
    {
        f32 h = height_for_age(p, age);
        f32 wind_penalty = 1.0f - 0.30f * out->wind_form_factor;
        /* In a closed canopy a tree invests in height rather than spread. */
        f32 canopy_bonus = 1.0f + 0.10f * env->canopy_closure;
        f32 individual = tg_rng_normal_clamped(&rng_form, 1.0f, 0.07f, 2.5f);
        out->height_m = tg_maxf(0.15f, h * wind_penalty * canopy_bonus
                                       * hf.vigor * individual);
    }

    /* --- crown ------------------------------------------------------------- */
    {
        /* Crown spread is suppressed by both wind and neighbours; the two act
         * together, which is why a forest tree on a ridge is markedly narrow. */
        f32 spread = p->crown_width_ratio
                   * (1.0f - 0.45f * env->canopy_closure)
                   * (1.0f - 0.25f * out->wind_form_factor);
        f32 individual = tg_rng_normal_clamped(&rng_crown, 1.0f, 0.09f, 2.5f);
        /* A young tree is proportionally narrower than a mature one. */
        f32 juvenile = tg_lerpf(0.55f, 1.0f, tg_smoothstepf(0.0f, 0.8f, out->maturity));
        out->crown_width_m = tg_maxf(0.10f,
            out->height_m * spread * individual * juvenile);
    }
    out->crown_base_height_m = out->height_m
        * tg_clampf(p->crown_base_height_ratio
                    /* Shade kills lower branches, lifting the live crown. This
                     * is the mechanism, not a cosmetic offset. */
                    + 0.42f * env->canopy_closure, 0.02f, 0.80f);
    out->crown_widest_height_m = tg_lerpf(out->crown_base_height_m, out->height_m,
                                          tg_clampf(p->crown_widest_at, 0.02f, 0.98f));

    /* --- crown asymmetry and displacement ---------------------------------
     * Asymmetry is CAUSAL: it comes from light anisotropy, wind and health, and
     * its DIRECTION is the resultant of moving toward light and away from wind.
     * Random jitter alone would read as noise rather than history. */
    {
        V3 toward_light = v3_norm_or(v3(env->light_direction.x, 0.0f,
                                        env->light_direction.z), v3(1, 0, 0));
        V3 downwind = v3_norm_or(v3(env->wind_direction.x, 0.0f,
                                    env->wind_direction.z), v3(1, 0, 0));
        f32 wl = env->light_anisotropy;
        f32 ww = out->wind_form_factor;
        V3 combined = v3_add(v3_scale(toward_light, wl), v3_scale(downwind, ww));
        out->crown_offset_dir = v3_norm_or(combined, toward_light);
        out->crown_asymmetry = tg_saturatef(0.55f * wl + 0.35f * ww + hf.asymmetry);
    }

    /* --- trunk ------------------------------------------------------------- */
    out->basal_thickening = 1.0f + 0.35f * out->wind_form_factor;
    {
        /* A first-order estimate from height and taper, refined later by the
         * pipe-model pass over the actual supported structure. Present here so
         * that attractor placement and the root system have a trunk scale to
         * work from before the graph exists. */
        f32 slenderness = tg_lerpf(60.0f, 95.0f, env->canopy_closure);
        out->trunk_base_radius_m =
            tg_maxf(p->tip_radius_m * 2.0f,
                    out->height_m / slenderness * 0.5f * out->basal_thickening);
    }
    {
        /* Lean: small, biased downwind, and larger on exposed sites. */
        f32 base_lean = tg_rng_normal_clamped(&rng_lean, 0.0f, 0.020f, 2.0f);
        f32 wind_lean = 0.10f * out->wind_form_factor;
        V3 downwind = v3_norm_or(v3(env->wind_direction.x, 0.0f,
                                    env->wind_direction.z), v3(1, 0, 0));
        f32 azimuth = tg_rng_f32(&rng_lean) * TG_TAU_F;
        V3 random_dir = v3(cosf(azimuth), 0.0f, sinf(azimuth));
        out->lean_angle_rad = tg_absf(base_lean) + wind_lean;
        out->lean_direction = v3_norm_or(
            v3_add(v3_scale(downwind, wind_lean),
                   v3_scale(random_dir, tg_absf(base_lean))), downwind);
    }

    /* --- apical control decays with age (Rauh becomes decurrent) ----------- */
    out->apical_control = tg_maxf(p->apical_control_min,
                                  p->apical_control
                                  - p->apical_control_decay_per_year * age);

    /* --- bark maturity ----------------------------------------------------
     * Driven by age AND trunk size together: a fast-grown young tree with a
     * thick trunk is not as furrowed as an old slow one, and neither is smooth. */
    {
        f32 by_age = tg_smoothstepf(0.04f, 0.55f, out->maturity);
        f32 by_size = tg_smoothstepf(0.03f, 0.30f, out->trunk_base_radius_m);
        out->bark_maturity = tg_saturatef(0.65f * by_age + 0.35f * by_size);
    }

    /* --- deadwood and foliage --------------------------------------------- */
    out->deadwood_fraction = tg_saturatef(
        hf.deadwood
        + 0.30f * env->canopy_closure      /* interior shading kills shoots   */
        + 0.12f * out->wind_form_factor    /* windward tip dieback            */
        + 0.15f * tg_smoothstepf(0.8f, 2.0f, out->maturity)); /* over-mature  */

    out->leaf_scale = hf.leaf_scale
                    /* Shade leaves are larger and thinner; sun leaves smaller. */
                    * tg_lerpf(1.0f, 1.18f, env->canopy_closure)
                    * tg_lerpf(1.0f, 0.85f, out->wind_form_factor);
    out->foliage_density = hf.foliage;
    if (settings->season == SEASON_WINTER && p->deciduous) {
        out->foliage_density = 0.0f;
    } else if (settings->season == SEASON_SPRING) {
        out->foliage_density *= 0.75f;
        out->leaf_scale *= 0.70f;
    } else if (settings->season == SEASON_AUTUMN && p->deciduous) {
        out->foliage_density *= 0.80f;
    }

    /* --- roots -------------------------------------------------------------
     *
     * Root spread is scaled from CROWN WIDTH, not from height, so the root plate
     * relates to the structure it actually supports. Scaling from height was
     * measured to give a 61.9 m root radius around a 4.9 m conifer crown -- a
     * 12:1 ratio that no temperate tree exhibits. Field guidance puts the
     * structural root plate at roughly one to three times the crown radius. */
    out->root_spread_m = out->crown_width_m * 0.5f * p->root_spread_ratio
                       /* Compacted soil forces a shallower, wider system. */
                       * tg_lerpf(1.0f, 1.25f, env->soil_resistance);
    out->root_depth_m = out->height_m * p->root_depth_ratio
                      * tg_lerpf(1.0f, 0.55f, env->soil_resistance);

    /* --- growth steps -----------------------------------------------------
     * One step per flush. Capped so that an extreme age cannot make generation
     * unbounded; the cap is reported through growth_steps rather than hidden. */
    {
        f32 steps = age * (f32)tg_max_u32(p->flushes_per_year, 1u);
        out->growth_steps = tg_clamp_u32((u32)(steps + 0.5f), 3u, 400u);
    }

    /* --- budgets ----------------------------------------------------------- */
    out->cross_section_segments_min = qb.seg_min;
    out->cross_section_segments_max = qb.seg_max;
    out->max_leaves = qb.max_leaves;
    out->leaf_triangle_budget = qb.leaf_tris;
    out->bark_feature_size_m = qb.bark_feature_m;
    out->max_organs = qb.max_organs;

    return TG_OK;
}
