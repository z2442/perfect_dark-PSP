#!/bin/bash

set -euo pipefail

BUILD_TYPE_WAS_SET="${BUILD_TYPE+x}"
BUILD_PRX_WAS_SET="${BUILD_PRX+x}"

BUILD_DIR="${BUILD_DIR:-build-psp}"
ROMID="${ROMID:-ntsc-final}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_PRX="${BUILD_PRX:-1}"
PSP_ENABLE_GPROF="${PSP_ENABLE_GPROF:-0}"

if [[ "${GPROF:-0}" == "1" ]]; then
    PSP_ENABLE_GPROF=1

    # The PSP gprof samples are built as plain EBOOTs, not PRX modules.
    # Prefer that path for profiling unless the caller explicitly overrides it.
    if [[ -z "${BUILD_PRX_WAS_SET}" ]]; then
        BUILD_PRX=0
    fi

    # Keep symbols in the ELF that psp-gprof reads.
    if [[ -z "${BUILD_TYPE_WAS_SET}" && "${BUILD_TYPE}" == "Release" ]]; then
        BUILD_TYPE="RelWithDebInfo"
    fi

    if [[ "${BUILD_DIR}" == "build-psp" ]]; then
        BUILD_DIR="build-psp-gprof"
    fi
fi

# Remove old build directory
rm -rf "${BUILD_DIR}"

# Generate build system files for PSP
# -DCMAKE_BUILD_TYPE=Debug to build the debug with symbols aka larger 
psp-cmake -S . -B "${BUILD_DIR}" \
    -DBUILD_PSP=ON \
    -DROMID="${ROMID}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DBUILD_PRX="${BUILD_PRX}" \
    -DPSP_ENABLE_GPROF="${PSP_ENABLE_GPROF}"

# Build 
cmake --build "${BUILD_DIR}" -j 4
cmake --install "${BUILD_DIR}" -j 4 --prefix "$PWD"
