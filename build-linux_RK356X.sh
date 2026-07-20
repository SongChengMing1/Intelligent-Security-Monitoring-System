set -e

TARGET_SOC="rk356x"
TOOL_CHAIN=${TOOL_CHAIN:-}
CROSS_COMPILE=${CROSS_COMPILE:-}
if [[ -z "${CROSS_COMPILE}" && -n "${TOOL_CHAIN}" ]]; then
  CROSS_COMPILE=${TOOL_CHAIN}/bin/aarch64-linux-gnu
fi
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu}

if [[ -n "${TOOL_CHAIN}" && -d "${TOOL_CHAIN}/lib64" ]]; then
  export LD_LIBRARY_PATH="${TOOL_CHAIN}/lib64${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
export CC="${CC:-${CROSS_COMPILE}-gcc}"
export CXX="${CXX:-${CROSS_COMPILE}-g++}"

ROOT_PWD=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" && pwd )
GST_SYSROOT=${GST_SYSROOT:-${ROOT_PWD}/third_party/sysroots/rk3568-bullseye}
if [[ ! -d "${GST_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig" ]]; then
  echo "GStreamer sysroot is required: ${GST_SYSROOT}" >&2
  exit 1
fi
export PKG_CONFIG_SYSROOT_DIR=${GST_SYSROOT}
export PKG_CONFIG_LIBDIR=${GST_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${GST_SYSROOT}/usr/share/pkgconfig

# build
BUILD_DIR=${ROOT_PWD}/build/build_linux_aarch64

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] && ! grep -q "${ROOT_PWD}" "${BUILD_DIR}/CMakeCache.txt"; then
  rm -rf "${BUILD_DIR}"
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  mkdir -p ${BUILD_DIR}
fi

cd ${BUILD_DIR}
cmake ../.. -DCMAKE_SYSTEM_NAME=Linux -DTARGET_SOC=${TARGET_SOC}
make -j4
make install
cd -
