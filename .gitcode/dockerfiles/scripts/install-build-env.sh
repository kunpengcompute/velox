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

set -euxo pipefail

readonly BUILDTOOLS=/opt/buildtools
readonly CMAKE_VERSION=3.28.2
readonly CMAKE_HOME="${BUILDTOOLS}/cmake-${CMAKE_VERSION}-linux-aarch64"
readonly VELOX_REPO=https://gitcode.com/boostkit/velox.git
readonly VELOX_BRANCH=${VELOX_BRANCH:-gluten_1.3.0_velox_930}
readonly VELOX_3RDPARTY_URL=https://boostkit-bigdata-public.obs.cn-north-4.myhuaweicloud.com/buildcache/velox/3rdparty_rely/velox_3rdparty_rely.tar.gz

install_system_tools() {
  dnf update -y
  dnf install -y -q --setopt=install_weak_deps=False autoconf automake binutils binutils-devel bison ccache cmake dos2unix double-conversion-devel doxygen elfutils-libelf-devel flex gcc gcc-c++ git glog-devel libcurl-devel libdwarf-devel libevent-devel libicu-devel libsodium-devel libtool lld lz4-devel make ninja-build openssl-devel patchelf perf python3-devel python3-pip re2-devel sudo texinfo tree which wget zip zlib-devel zstd-devel
  pip3 install --no-cache-dir pre-commit sphinx sphinx-tabs breathe sphinx_rtd_theme pandas tqdm openpyxl pyinstaller fastcov -i http://mirrors.aliyun.com/pypi/simple/ --trusted-host mirrors.aliyun.com
  dnf reinstall -y --allowerasing glibc-common
  locale
  dnf clean all
  rm -rf /var/cache/dnf
}

install_bisheng_jdks() {
  mkdir -p "${BUILDTOOLS}"
  pushd "${BUILDTOOLS}"
  wget https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_jdk/bisheng-jdk-17.0.18-b13-linux-aarch64.tar.gz
  tar -xf bisheng-jdk-17.0.18-b13-linux-aarch64.tar.gz
  rm -f bisheng-jdk-17.0.18-b13-linux-aarch64.tar.gz
  wget https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_jdk/bisheng-jdk-8u342-linux-aarch64.tar.gz
  tar -xf bisheng-jdk-8u342-linux-aarch64.tar.gz
  rm -f bisheng-jdk-8u342-linux-aarch64.tar.gz
  popd
}

install_cmake() {
  pushd "${BUILDTOOLS}"
  wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz"
  tar -xf "cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz"
  rm -f "cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz"
  rm -rf /usr/local/bin/cmake /usr/local/share/cmake-${CMAKE_VERSION%.*}
  ln -s "${CMAKE_HOME}/bin/cmake" /usr/local/bin/cmake
  ln -s "${CMAKE_HOME}/share/cmake-${CMAKE_VERSION%.*}" /usr/local/share/cmake-${CMAKE_VERSION%.*}
  popd
}

install_velox_dependencies() {
  cd /tmp || exit 1
  git clone -b "${VELOX_BRANCH}" "${VELOX_REPO}"
  pushd velox
  export PATH="${CMAKE_HOME}/bin:${PATH}"
  ln -snf "${CMAKE_HOME}/bin/cmake" /usr/bin/cmake
  rm -rf /usr/local/lib64/libgtest.a /usr/local/lib64/libgtest_main.a /usr/local/lib64/libsnappy.*
  rm -rf /usr/local/lib64/cmake/GTest /usr/local/lib64/cmake/GMock /usr/local/lib64/cmake/Snappy
  rm -rf /usr/local/include/gtest /usr/local/include/gmock
  wget -q "${VELOX_3RDPARTY_URL}"
  tar -xf velox_3rdparty_rely.tar.gz
  VELOX_TARBALL_DIR="$(pwd)/velox_3rdparty_rely"
  export VELOX_TARBALL_DIR
  export DEPENDENCY_DIR=/tmp/dependency_dir
  mkdir -p "${DEPENDENCY_DIR}"
  VELOX_HOME="$(pwd)"
  export VELOX_HOME
  source ./scripts/velox_tarball_profile.sh
  sudo -E bash scripts/setup-openeuler.sh
  popd
}

main() {
  cd /tmp || exit 1
  install_system_tools
  install_bisheng_jdks
  install_cmake
  install_velox_dependencies
  rm -rf /tmp/*
}

main "$@"
