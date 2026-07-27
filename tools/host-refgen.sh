#!/usr/bin/env bash
# host-refgen.sh -- builds and runs the headless reference generator.
#
# WHAT THIS IS
#   A validation instrument. It compiles the portable engine layers together with
#   tools/refgen.c, tools/swrast.c and tools/img_png.c, runs the real generation
#   pipeline, validates the result and writes PNG captures plus OBJ files into
#   artifacts/.
#
#   It exists because the project directive requires visual quality to be judged
#   visually and this development environment has no GPU and no display. The
#   shipped renderer is Direct3D 12; nothing here is part of it.
#
# SAFETY
#   Writes only inside build-host/ and artifacts/. No network. No installation.

set -o errexit
set -o nounset
set -o pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-host/refgen"
OUT_DIR="${ROOT_DIR}/artifacts"

CC="${CC:-cc}"
MODE="${1:-release}"

SOURCES=(
    "src/core/log.c" "src/core/mem.c" "src/core/math3d.c" "src/core/hash.c"
    "src/core/rng.c"
    "src/geom/mesh.c" "src/geom/mesh_validate.c" "src/geom/spatial.c"
    "src/geom/camera.c"
    "src/tree/tree_profile.c" "src/tree/tree_graph.c" "src/tree/tree_growth.c"
    "src/tree/tree_mechanics.c" "src/tree/tree_skin.c"
    "src/tree/tree_foliage.c"
    "tools/img_png.c" "tools/swrast.c" "tools/refgen.c"
)

WARN_FLAGS=(
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion
    -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-align
    -Wwrite-strings -Wundef -Wvla -Wswitch-enum
)
COMMON=(-std=c17 -pedantic -fno-fast-math "${WARN_FLAGS[@]}")

case "${MODE}" in
    debug)   BUILD=(-O0 -g3 -DTG_DEBUG=1) ;;
    release) BUILD=(-O2 -g -DTG_DEBUG=0 -DNDEBUG) ;;
    *) echo "ERROR: unknown mode '${MODE}'. Use debug or release." >&2; exit 1 ;;
esac

mkdir -p "${BUILD_DIR}" "${OUT_DIR}"
rm -f "${BUILD_DIR}/refgen"

FULL=()
for s in "${SOURCES[@]}"; do
    if [[ ! -f "${ROOT_DIR}/${s}" ]]; then
        echo "ERROR: expected source missing: ${s}" >&2
        exit 1
    fi
    FULL+=("${ROOT_DIR}/${s}")
done

echo "Building reference generator (${MODE})..."
"${CC}" "${COMMON[@]}" "${BUILD[@]}" "${FULL[@]}" -o "${BUILD_DIR}/refgen" -lm

if [[ ! -x "${BUILD_DIR}/refgen" ]]; then
    echo "ERROR: build produced no executable." >&2
    exit 1
fi

echo
cd "${ROOT_DIR}"
"${BUILD_DIR}/refgen" "artifacts"
