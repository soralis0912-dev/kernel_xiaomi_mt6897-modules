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
