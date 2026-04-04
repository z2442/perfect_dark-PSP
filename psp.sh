#!/bin/bash

set -euo pipefail

BUILD_DIR_WAS_SET="${BUILD_DIR+x}"
BUILD_TYPE_WAS_SET="${BUILD_TYPE+x}"
BUILD_PRX_WAS_SET="${BUILD_PRX+x}"

BUILD_DIR="${BUILD_DIR:-}"
ROMID="${ROMID:-ntsc-final}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_PRX="${BUILD_PRX:-1}"
PSP_ENABLE_GPROF="${PSP_ENABLE_GPROF:-0}"
PSP_AUDIO_ME="${PSP_AUDIO_ME:-0}"

case "${PSP_AUDIO_ME}" in
    1|ON|on|TRUE|true|YES|yes)
        PSP_AUDIO_ME=ON
        ;;
    0|OFF|off|FALSE|false|NO|no|"")
        PSP_AUDIO_ME=OFF
        ;;
    *)
        echo "Unsupported PSP_AUDIO_ME value: ${PSP_AUDIO_ME}" >&2
        exit 1
        ;;
esac

if [[ -z "${BUILD_DIR_WAS_SET}" ]]; then
    if [[ "${PSP_AUDIO_ME}" == "ON" ]]; then
        BUILD_DIR="build-psp-me"
    else
        BUILD_DIR="build-psp"
    fi
fi

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

    if [[ -z "${BUILD_DIR_WAS_SET}" ]]; then
        if [[ "${PSP_AUDIO_ME}" == "ON" ]]; then
            BUILD_DIR="build-psp-me-gprof"
        else
            BUILD_DIR="build-psp-gprof"
        fi
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
    -DPSP_ENABLE_GPROF="${PSP_ENABLE_GPROF}" \
    -DPSP_AUDIO_ME="${PSP_AUDIO_ME}"

# Build 
cmake --build "${BUILD_DIR}" -j 4
cmake --install "${BUILD_DIR}" -j 4 --prefix "$PWD"
