#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# shellcheck disable=SC2034,SC2154
# usage: sh build.sh {compile|ut|package}
set -euxo pipefail

BUILD_SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly BUILD_SCRIPT_DIR
# shellcheck source=init_env.sh
source "${BUILD_SCRIPT_DIR}/init_env.sh"

VELOX_HOME=$(cd "${REPO_ROOT}/.." && pwd)
readonly VELOX_HOME
WORKSPACE=${WORKSPACE:-$(dirname "${VELOX_HOME}")}
readonly WORKSPACE
readonly CMAKE_HOME=${CMAKE_HOME:-/opt/buildtools/cmake-3.28.2-linux-aarch64}
readonly THIRDPARTY_URL=https://boostkit-bigdata-public.obs.cn-north-4.myhuaweicloud.com/buildcache/velox/3rdparty_rely/velox_3rdparty_rely.tar.gz
readonly DEPENDENCY_DIR=${DEPENDENCY_DIR:-/tmp/velox}
readonly NUM_THREADS=${NUM_THREADS:-16}
readonly GCC_TOOLSET_LIB=/opt/openEuler/gcc-toolset-12/root/usr/lib64

usage() {
  echo "usage: $0 {compile|ut|package}" >&2
  exit 2
}

# 22.03 镜像用 gcc-toolset-12 编译，其 libstdc++ 不在默认搜索路径，sudo 又会丢弃 LD_LIBRARY_PATH
function setup_toolset_lib() {
  [[ -d ${GCC_TOOLSET_LIB} ]] || return 0
  export LD_LIBRARY_PATH="${GCC_TOOLSET_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}

function prepare() {
  [[ -f "${VELOX_HOME}/scripts/build_velox.sh" ]] || {
    echo "ERROR: ${VELOX_HOME}/scripts/build_velox.sh 不存在，构建分支需包含该脚本" >&2
    exit 1
  }
  ln -snf "${CMAKE_HOME}/bin/cmake" /usr/bin/cmake
  export PATH="${CMAKE_HOME}/bin:${PATH}"
  set_ccache
  setup_toolset_lib
  ccache -z >/dev/null 2>&1 || true
  cd "${VELOX_HOME}"
  [[ -f velox_3rdparty_rely.tar.gz ]] || wget -q "${THIRDPARTY_URL}"
  [[ -d velox_3rdparty_rely ]] || tar -xf velox_3rdparty_rely.tar.gz
  export VELOX_TARBALL_DIR="${VELOX_HOME}/velox_3rdparty_rely"
  export DEPENDENCY_DIR VELOX_HOME NUM_THREADS
  source ./scripts/velox_tarball_profile.sh
}

function build_velox() {
  sudo -E env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" bash scripts/build_velox.sh \
    --velox_home="${VELOX_HOME}" \
    --enable_hdfs=ON \
    --num_threads="${NUM_THREADS}"
}

function ut_velox() {
  sudo -E env LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" bash scripts/build_velox.sh \
    --velox_home="${VELOX_HOME}" \
    --enable_hdfs=ON \
    --build_tests=ON \
    --num_threads="${NUM_THREADS}"
  cd "${VELOX_HOME}/_build/release"
  ctest --output-on-failure -j "$(nproc)"
}

function BoostKit_velox_Version_info() {
  touch version.txt
  echo "Product Name: Kunpeng BoostKit" >>version.txt
  echo "Product Version: ${Product_Version}" >>version.txt
  echo "Component Name: BoostKit-velox" >>version.txt
  echo "Component Version: ${velox_Version}" >>version.txt
}

function dependencies_lib_dopackage_openEuler22.03() {
  mkdir -p ${VELOX_HOME}/Dependency_library_velox/Dependency_library_openEuler22.03
  pushd ${VELOX_HOME}/Dependency_library_velox/Dependency_library_openEuler22.03
  cp -f /usr/local/lib/libboost_atomic.so.1.84.0 ./libboost_atomic.so.1.84.0
  cp -f /usr/local/lib/libboost_context.so.1.84.0 ./libboost_context.so.1.84.0
  cp -f /usr/local/lib/libboost_filesystem.so.1.84.0 ./libboost_filesystem.so.1.84.0
  cp -f /usr/local/lib/libboost_program_options.so.1.84.0 ./libboost_program_options.so.1.84.0
  cp -f /usr/local/lib/libboost_regex.so.1.84.0 ./libboost_regex.so.1.84.0
  cp -f /usr/local/lib/libboost_system.so.1.84.0 ./libboost_system.so.1.84.0
  cp -f /usr/local/lib/libboost_thread.so.1.84.0 ./libboost_thread.so.1.84.0
  cp -f /usr/lib64/libcrypto.so.1.1.1wa ./libcrypto.so.1.1
  cp -f /usr/lib64/libdouble-conversion.so.3.1.5 ./libdouble-conversion.so.3
  cp -f /usr/lib64/libdwarf.so.0.9.1 ./libdwarf.so.0
  cp -f /usr/lib64/libevent-2.1.so.7.0.1 ./libevent-2.1.so.7
  cp -f /opt/openEuler/gcc-toolset-12/root/usr/lib64/libgcc_s-12.so.1 ./libgcc_s.so.1
  cp -f /usr/local/lib64/libgflags.so.2.2.2 ./libgflags.so.2.2
  cp -f /usr/local/lib64/libglog.so.0.6.0 ./libglog.so.1
  cp -f /usr/lib64/libicudata.so.72.1 ./libicudata.so.72
  cp -f /usr/lib64/libicui18n.so.72.1 ./libicui18n.so.72
  cp -f /usr/lib64/libicuuc.so.72.1 ./libicuuc.so.72
  cp -f /usr/lib64/liblz4.so.1.9.3 ./liblz4.so.1
  cp -f /usr/lib64/liblzma.so.5.2.5 ./liblzma.so.5
  cp -f /usr/lib64/libre2.so.9.0.0 ./libre2.so.9
  cp -f /usr/lib64/libsodium.so.23.3.0 ./libsodium.so.23
  cp -f /usr/lib64/libssl.so.1.1.1wa ./libssl.so.1.1
  cp -f /opt/openEuler/gcc-toolset-12/root/usr/lib64/libstdc++.so.6.0.30 ./libstdc++.so.6
  cp -f /usr/lib64/libz.so.1.2.11 ./libz.so.1
  cp -f /usr/lib64/libzstd.so.1.5.0 ./libzstd.so.1
  popd
  pushd ${VELOX_HOME} && zip -r Dependency_library_velox.zip Dependency_library_velox && popd
}

function dependencies_lib_dopackage_openEuler24.03() {
  mkdir -p ${VELOX_HOME}/Dependency_library_velox/Dependency_library_openEuler24.03
  pushd ${VELOX_HOME}/Dependency_library_velox/Dependency_library_openEuler24.03
  cp -f /usr/lib64/libcrypto.so.3.0.12 ./libcrypto.so.3
  cp -f /usr/lib64/libicudata.so.74.1 ./libicudata.so.74
  cp -f /usr/lib64/libicuuc.so.74.1 ./libicuuc.so.74
  cp -f /usr/lib64/libicui18n.so.74.1 ./libicui18n.so.74
  cp -f /usr/lib64/liblz4.so.1.9.4 ./liblz4.so.1
  cp -f /usr/lib64/liblzma.so.5.4.7 ./liblzma.so.5
  cp -f /usr/lib64/libre2.so.11.0.0 ./libre2.so.11
  cp -f /usr/lib64/libsodium.so.26.1.0 ./libsodium.so.26
  cp -f /usr/lib64/libssl.so.3.0.12 ./libssl.so.3
  cp -f /usr/lib64/libz.so.1.2.13 ./libz.so.1
  cp -f /usr/lib64/libzstd.so.1.5.5 ./libzstd.so.1
  cp -f /usr/lib64/libstdc++.so.6.0.30 ./libstdc++.so.6
  cp -f /usr/lib64/libgcc_s-12.so.1 ./libgcc_s.so.1
  cp -f /usr/local/lib/libboost_atomic.so.1.84.0 ./libboost_atomic.so.1.84.0
  cp -f /usr/local/lib/libboost_context.so.1.84.0 ./libboost_context.so.1.84.0
  cp -f /usr/local/lib/libboost_filesystem.so.1.84.0 ./libboost_filesystem.so.1.84.0
  cp -f /usr/local/lib/libboost_program_options.so.1.84.0 ./libboost_program_options.so.1.84.0
  cp -f /usr/local/lib/libboost_regex.so.1.84.0 ./libboost_regex.so.1.84.0
  cp -f /usr/local/lib/libboost_system.so.1.84.0 ./libboost_system.so.1.84.0
  cp -f /usr/local/lib/libboost_thread.so.1.84.0 ./libboost_thread.so.1.84.0
  cp -f /usr/lib64/libdouble-conversion.so.3.1.5 ./libdouble-conversion.so.3
  cp -f /usr/lib64/libdwarf.so.0.9.1 ./libdwarf.so.0
  cp -f /usr/lib64/libevent-2.1.so.7.0.1 ./libevent-2.1.so.7
  cp -f /usr/local/lib64/libgflags.so.2.2.2 ./libgflags.so.2.2
  cp -f /usr/local/lib64/libglog.so.0.6.0 ./libglog.so.1
  popd
  pushd ${VELOX_HOME} && zip -r Dependency_library_velox.zip Dependency_library_velox && popd
}

function package_velox() {
  local pkg="boostkit-omniruntime-velox_${velox_Version}"
  local agentpath="${WORKSPACE}/output/${module}"
  mkdir -p "${agentpath}"

  ID=openEuler
  VERSION_ID=$(awk '{print $3}' /etc/openEuler-release)
  dependencies_lib_dopackage_${ID}${VERSION_ID}
  cp -f ${VELOX_HOME}/Dependency_library_velox.zip "${agentpath}/"

  BoostKit_velox_Version_info
  cd "${VELOX_HOME}"
  zip -r "${pkg}.zip" _build version.txt
  cp -f "${pkg}.zip" "${agentpath}/"

  cd "${agentpath}"
  sha256sum ${pkg}.zip >${pkg}.zip.sha256sum
  sha256sum Dependency_library_velox.zip >Dependency_library_velox.zip.sha256sum
  python3 "${VELOX_HOME}/.gitcode/scripts/collect_software_info.py" "${WORKSPACE}" Dependency_library_velox.zip
  python3 "${VELOX_HOME}/.gitcode/scripts/collect_software_info.py" "${WORKSPACE}" "${pkg}.zip"
}

case "${1:-}" in
compile)
  prepare
  build_velox
  ;;
ut)
  prepare
  ut_velox
  ;;
package)
  prepare
  build_velox
  package_velox
  ;;
*)
  usage
  ;;
esac
