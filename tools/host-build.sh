#!/usr/bin/env bash
# host-build.sh -- verification build for the PORTABLE layers of Tree_G.
#
# WHAT THIS IS
#   The shipped application targets Windows + MSVC + D3D12 and is built with
#   build-and-run.cmd. This script exists so that the portable layers -- core,
#   geometry, and the whole tree generator -- can be compiled and exercised on a
#   non-Windows development machine. It is a verification instrument, not a
#   second product build.
#
# WHAT IT DOES NOT DO
#   It does not compile src/render/, src/app/, or any HLSL. Those require the
#   Windows SDK. This is deliberate: the layering rule is enforced by the fact
#   that this build has no Windows headers available at all, so an accidental
#   platform dependency in a portable layer fails here immediately.
#
# USAGE
#   tools/host-build.sh              debug build + run tests
#   tools/host-build.sh release      optimised build + run tests
#   tools/host-build.sh asan         debug + address/UB sanitizers + run tests
#   tools/host-build.sh <mode> --no-run
#
# SAFETY
#   Writes only inside build-host/. Removes only files it created there.
#   No network access. No installation. No changes outside the repository.

set -o errexit
set -o nounset
set -o pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-host"

MODE="${1:-debug}"
RUN=1
for arg in "$@"; do
    if [[ "${arg}" == "--no-run" ]]; then RUN=0; fi
done

CC="${CC:-cc}"
if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "ERROR: C compiler '${CC}' not found." >&2
    exit 1
fi

# -------------------------------------------------------------------------
# Source lists. Kept explicit: a glob would silently pick up a stray file and
# quietly change what is being verified.
# -------------------------------------------------------------------------
CORE_SOURCES=(
    "src/core/log.c"
    "src/core/mem.c"
    "src/core/math3d.c"
    "src/core/hash.c"
    "src/core/rng.c"
)

GEOM_SOURCES=()
TREE_SOURCES=()

TEST_SOURCES=(
    "tests/test_main.c"
    "tests/test_mem.c"
    "tests/test_math.c"
    "tests/test_rng.c"
    "tests/test_hash.c"
)

# -------------------------------------------------------------------------
# Flags.
#
# -Wall -Wextra plus the specific warnings that catch the defect classes this
# project cares about most: implicit conversions that silently lose geometry
# precision, missing prototypes that hide a layering violation, and shadowing.
#
# -Werror is on. The project requires a warning-free build.
#
# NOT used: -ffast-math or -Ofast. They would break the determinism contract by
# permitting reassociation and reciprocal substitution.
# -------------------------------------------------------------------------
WARN_FLAGS=(
    -Wall -Wextra -Werror
    -Wshadow
    -Wconversion -Wdouble-promotion
    -Wstrict-prototypes -Wmissing-prototypes
    -Wpointer-arith
    -Wcast-align
    -Wwrite-strings
    -Wundef
    -Wvla
    -Wswitch-enum
)

COMMON_FLAGS=(-std=c17 -pedantic -fno-fast-math "${WARN_FLAGS[@]}")
LINK_FLAGS=(-lm)

case "${MODE}" in
    debug)
        BUILD_FLAGS=(-O0 -g3 -DTG_DEBUG=1)
        OUT_NAME="tree_g_tests_debug"
        ;;
    release)
        BUILD_FLAGS=(-O2 -g -DTG_DEBUG=0 -DNDEBUG)
        OUT_NAME="tree_g_tests_release"
        ;;
    asan)
        BUILD_FLAGS=(-O1 -g3 -DTG_DEBUG=1
                     -fsanitize=address,undefined
                     -fno-omit-frame-pointer
                     -fno-sanitize-recover=all)
        LINK_FLAGS+=(-fsanitize=address,undefined)
        OUT_NAME="tree_g_tests_asan"
        ;;
    *)
        echo "ERROR: unknown mode '${MODE}'. Use debug, release, or asan." >&2
        exit 1
        ;;
esac

OBJ_DIR="${BUILD_DIR}/${MODE}/obj"
BIN="${BUILD_DIR}/${MODE}/${OUT_NAME}"

echo "Tree_G host verification build"
echo "  compiler : $(${CC} --version | head -n 1)"
echo "  mode     : ${MODE}"
echo "  output   : ${BIN}"
echo

mkdir -p "${OBJ_DIR}"
# Remove only object files and the binary from OUR build directory.
find "${OBJ_DIR}" -maxdepth 1 -name '*.o' -type f -delete
rm -f "${BIN}"

ALL_SOURCES=("${CORE_SOURCES[@]}" ${GEOM_SOURCES[@]+"${GEOM_SOURCES[@]}"} \
             ${TREE_SOURCES[@]+"${TREE_SOURCES[@]}"} "${TEST_SOURCES[@]}")

OBJECTS=()
FAILED=0
for src in "${ALL_SOURCES[@]}"; do
    if [[ ! -f "${ROOT_DIR}/${src}" ]]; then
        echo "ERROR: expected source file missing: ${src}" >&2
        exit 1
    fi
    obj="${OBJ_DIR}/$(echo "${src}" | tr '/' '_' | sed 's/\.c$/.o/')"
    printf '  CC  %s\n' "${src}"
    if ! "${CC}" "${COMMON_FLAGS[@]}" "${BUILD_FLAGS[@]}" \
            -c "${ROOT_DIR}/${src}" -o "${obj}"; then
        FAILED=1
        break
    fi
    OBJECTS+=("${obj}")
done

if [[ ${FAILED} -ne 0 ]]; then
    echo
    echo "BUILD FAILED (compilation). No executable produced." >&2
    exit 1
fi

printf '  LD  %s\n' "${BIN}"
if ! "${CC}" "${BUILD_FLAGS[@]}" "${OBJECTS[@]}" -o "${BIN}" "${LINK_FLAGS[@]}"; then
    echo
    echo "BUILD FAILED (link). No executable produced." >&2
    exit 1
fi

if [[ ! -x "${BIN}" ]]; then
    echo "BUILD FAILED: expected executable was not produced." >&2
    exit 1
fi

echo
echo "Build succeeded."

if [[ ${RUN} -eq 0 ]]; then
    exit 0
fi

echo "Running tests..."
echo
set +o errexit
"${BIN}"
RC=$?
set -o errexit
echo
if [[ ${RC} -eq 0 ]]; then
    echo "TESTS PASSED (${MODE})"
else
    echo "TESTS FAILED (${MODE}), exit code ${RC}" >&2
fi
exit ${RC}
