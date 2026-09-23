#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

TARGET_DEVICE="mgk_64_k61"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${ROOT_DIR}"

KERNEL_MODULES_DIR="$(find "${ROOT_DIR}" -maxdepth 1 -type d -name 'kernel_device_modules-*' | sort -V | tail -n1)"
if [ -z "${KERNEL_MODULES_DIR}" ] || [ ! -f "${KERNEL_MODULES_DIR}/build.sh" ]; then
    echo "error: no kernel_device_modules-*/build.sh found for MGKI build" >&2
    exit 1
fi
KERNEL_MODULES_NAME="$(basename "${KERNEL_MODULES_DIR}")"

MODE="${MODE:-user}"
export MODE

# Stamp the build. Without --config=stamp kleaf takes two shortcuts that both
# end up in /proc/version: stamp.bzl hardcodes the scmversion to
# "-maybe-dirty", and _setup_env.sh gets no KLEAF_SOURCE_DATE_EPOCHS, so it
# falls back to `git log` in ${KERNEL_DIR} - which build.config.mtk.aarch64
# sets to kernel-5.15, a directory that does not exist in an MGKI tree. That
# git call returns nothing, SOURCE_DATE_EPOCH becomes 0, and the kernel is
# stamped Thu Jan 1 00:00:00 UTC 1970.
#
# build.sh has no hook of its own; it passes DEBUG_ARGS and SANDBOX_ARGS to
# bazel verbatim, and only sets them itself under DEBUG=1 and SANDBOX=0.
export SANDBOX_ARGS="${SANDBOX_ARGS:+${SANDBOX_ARGS} }--config=stamp"

# _setup_env.sh prepends ROOT_DIR twice if out/ doesn't exist yet
export OUT_DIR="${ROOT_DIR}/out"
mkdir -p "${OUT_DIR}"

INTERMEDIATE_DIST="${OUT_DIR}/dist"
FINAL_DIST="${OUT_DIR}/${TARGET_DEVICE}/dist"

rm -rf "${INTERMEDIATE_DIST}" "${FINAL_DIST}"

bash "${KERNEL_MODULES_DIR}/build.sh"

SRC="${INTERMEDIATE_DIST}/${KERNEL_MODULES_NAME}"
if [ ! -d "${SRC}" ]; then
    echo "error: MGKI dist output not found under ${SRC}" >&2
    exit 1
fi

mkdir -p "${FINAL_DIST}"

cp -a "${SRC}/${TARGET_DEVICE}_customer_modules_install.${MODE}/"*.ko "${FINAL_DIST}/"

find "${SRC}/${TARGET_DEVICE}_kernel_aarch64.${MODE}/" -name '*.ko' -exec cp -t "${FINAL_DIST}/" {} +
cp -a "${SRC}/${TARGET_DEVICE}_kernel_aarch64.${MODE}/"Image.* "${FINAL_DIST}/"

uapi="${SRC}/${TARGET_DEVICE}_merged_uapi_headers.${MODE}"
if [ -d "${uapi}" ]; then
    cp -a "${uapi}/"* "${FINAL_DIST}/"
fi

cp -a "${SRC}/${TARGET_DEVICE}.${MODE}/"*.dtb "${FINAL_DIST}/"
find "${SRC}/${TARGET_DEVICE}.${MODE}/" -name '*.ko' -exec cp -n -t "${FINAL_DIST}/" {} +

echo "MGKI kernel artifacts staged in ${FINAL_DIST}"
