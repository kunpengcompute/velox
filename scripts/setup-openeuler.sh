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

# This script documents setting up an openEuler host for Velox
# development.  Running it should make you ready to compile.
#
# Environment variables:
# * INSTALL_PREREQUISITES="N": Skip installation of packages for build.
# * PROMPT_ALWAYS_RESPOND="n": Automatically respond to interactive prompts.
#     Use "n" to never wipe directories.
# * VELOX_SETUP_<DEPENDENCY>_URL: Override the default archive URL for a dependency.
#     For example, VELOX_SETUP_BOOST_URL can point to a local file:/// tarball.
#
# You can also run individual functions below by specifying them as arguments:
# $ scripts/setup-openeuler.sh install_googletest install_fmt
#

set -efx -o pipefail
# Some of the packages must be build with the same compiler flags
# so that some low level types are the same size. Also, disable warnings.
SCRIPTDIR=$(dirname "${BASH_SOURCE[0]}")
source $SCRIPTDIR/setup-helper-functions.sh
NPROC=$(getconf _NPROCESSORS_ONLN)
export CXXFLAGS=$(get_cxx_flags) # Used by boost.
export CFLAGS=${CXXFLAGS//"-std=c++17"/} # Used by LZO.
CMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}"
VELOX_BUILD_SHARED=${VELOX_BUILD_SHARED:-"OFF"} #Build folly and gflags shared for use in libvelox.so.
BUILD_DUCKDB="${BUILD_DUCKDB:-true}"
USE_CLANG="${USE_CLANG:-false}"
SUDO="${SUDO:-"sudo --preserve-env"}"
export INSTALL_PREFIX=${INSTALL_PREFIX:-"/usr/local"}
DEPENDENCY_DIR=${DEPENDENCY_DIR:-$(pwd)/deps-download}

FB_OS_VERSION="v2024.07.01.00"
FMT_VERSION="10.1.1"
BOOST_VERSION="boost-1.84.0"
ARROW_VERSION="15.0.0"
STEMMER_VERSION="2.2.0"
DUCKDB_VERSION="v0.8.1"

function dnf_install {
  ${SUDO} dnf install -y --setopt=install_weak_deps=False "$@"
}

function get_dependency_url {
  local DEPENDENCY_NAME=$1
  local DEFAULT_URL=$2
  local OVERRIDE_VAR="VELOX_SETUP_${DEPENDENCY_NAME}_URL"
  local OVERRIDE_URL="${!OVERRIDE_VAR:-}"

  if [[ -n "${OVERRIDE_URL}" ]]; then
    echo -n "${OVERRIDE_URL}"
  else
    echo -n "${DEFAULT_URL}"
  fi
}

function wget_and_untar_dependency {
  local DEPENDENCY_NAME=$1
  local DEFAULT_URL=$2
  local DIR=$3
  local URL
  URL=$(get_dependency_url "${DEPENDENCY_NAME}" "${DEFAULT_URL}")
  echo "Using ${DEPENDENCY_NAME} URL: ${URL}"
  wget_and_untar "${URL}" "${DIR}"
}

function install_clang15 {
  dnf_install clang15 gcc-toolset-13-libatomic-devel
}

# Install packages required for build.
function install_build_prerequisites {
  #${SUDO} dnf update -y
  #dnf_install epel-release dnf-plugins-core # For ccache, ninja
  ${SUDO} dnf config-manager --set-enabled crb
  #${SUDO} dnf update -y
  dnf_install ninja-build cmake ccache git wget which
  dnf_install autoconf automake python3-devel libtool

  # pip install cmake==3.28.3

  if [[ ${USE_CLANG} != "false" ]]; then
    install_clang15
  fi
}

# Install dependencies from the package managers.
function install_velox_deps_from_dnf {
  dnf_install libevent-devel \
    openssl-devel re2-devel libzstd-devel lz4-devel double-conversion-devel \
    libdwarf-devel elfutils-libelf-devel curl-devel libicu-devel bison flex \
    libsodium-devel zlib-devel

  # install sphinx for doc gen
  pip install sphinx sphinx-tabs breathe sphinx_rtd_theme
}

function install_conda {
  dnf_install conda
}

function install_gflags {
  # Remove an older version if present.
  ${SUDO} dnf remove -y gflags
  wget_and_untar_dependency GFLAGS https://gitee.com/mirrors/gflags/repository/archive/v2.2.2.tar.gz gflags
  cmake_install_dir gflags -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=ON -DBUILD_gflags_LIB=ON -DLIB_SUFFIX=64
}

function install_glog {
  wget_and_untar_dependency GLOG https://gitee.com/mirrors/glog/repository/archive/v0.6.0.tar.gz glog
  cmake_install_dir glog -DBUILD_SHARED_LIBS=ON
}

function install_lzo {
  wget_and_untar_dependency LZO http://www.oberhumer.com/opensource/lzo/download/lzo-2.10.tar.gz lzo
  (
    cd ${DEPENDENCY_DIR}/lzo
    ./configure --prefix=${INSTALL_PREFIX} --enable-shared --disable-static --docdir=/usr/share/doc/lzo-2.10
    make "-j${NPROC}"
    make install
  )
}

function install_boost {
  wget_and_untar_dependency BOOST https://gitee.com/mirrors/boost/repository/archive/boost-1.84.0.tar.gz boost
  (
    cd ${DEPENDENCY_DIR}/boost
    if [[ ${USE_CLANG} != "false" ]]; then
      ./bootstrap.sh --prefix=${INSTALL_PREFIX} --with-toolset="clang-15"
      # Switch the compiler from the clang-15 toolset which doesn't exist (clang-15.jam) to
      # clang of version 15 when toolset clang-15 is used.
      # This reconciles the project-config.jam generation with what the b2 build system allows for customization.
      sed -i 's/using clang-15/using clang : 15/g' project-config.jam
      ${SUDO} ./b2 "-j${NPROC}" -d0 install threading=multi toolset=clang-15 --without-python
    else
      ./bootstrap.sh --prefix=${INSTALL_PREFIX}
      ${SUDO} ./b2 "-j${NPROC}" -d0 install threading=multi --without-python
    fi
  )
}

function install_snappy {
  wget_and_untar_dependency SNAPPY https://gitee.com/mirrors/Snappy_old1/repository/archive/1.1.8.tar.gz snappy
  cmake_install_dir snappy -DSNAPPY_BUILD_TESTS=OFF
}

function install_fmt {
  wget_and_untar_dependency FMT https://gitee.com/mirrors/fmt/repository/archive/${FMT_VERSION}.tar.gz fmt
  cmake_install_dir fmt -DFMT_TEST=OFF
}

function install_protobuf {
  wget_and_untar_dependency PROTOBUF https://mirrors.huaweicloud.com/protobuf/release/v21.8/protobuf-all-21.8.tar.gz protobuf
  (
    cd ${DEPENDENCY_DIR}/protobuf
    ./configure CXXFLAGS="-fPIC" --prefix=${INSTALL_PREFIX}
    make "-j${NPROC}"
    make install
    ldconfig
  )
}

function install_fizz {
  wget_and_untar_dependency FIZZ https://gitee.com/mirrors/Fizz/repository/archive/${FB_OS_VERSION}.tar.gz fizz
  cmake_install_dir fizz/fizz -DBUILD_TESTS=OFF
}

function install_folly {
  wget_and_untar_dependency FOLLY https://gitee.com/mirrors/folly/repository/archive/${FB_OS_VERSION}.tar.gz folly
  cmake_install_dir folly -DFOLLY_HAVE_INT128_T=ON
}

function install_wangle {
  wget_and_untar_dependency WANGLE https://gitee.com/other_repository/wangle/repository/archive/${FB_OS_VERSION}.tar.gz wangle
  cmake_install_dir wangle/wangle -DBUILD_TESTS=OFF
}

function install_fbthrift {
  wget_and_untar_dependency FBTHRIFT https://gitee.com/mirrors/fbthrift/repository/archive/${FB_OS_VERSION}.tar.gz fbthrift
  cmake_install_dir fbthrift -Denable_tests=OFF -DBUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF
}

function install_mvfst {
  wget_and_untar_dependency MVFST https://gitee.com/mirrors_facebookincubator/mvfst/repository/archive/${FB_OS_VERSION}.tar.gz mvfst
  cmake_install_dir mvfst -DBUILD_TESTS=OFF
}

function install_duckdb {
  if $BUILD_DUCKDB ; then
    echo 'Building DuckDB'
    wget_and_untar_dependency DUCKDB https://gitee.com/mirrors/duckdb/repository/archive/${DUCKDB_VERSION}.tar.gz duckdb
    cmake_install_dir duckdb -DBUILD_UNITTESTS=OFF -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF -DBUILD_SHELL=OFF -DEXPORT_DLL_SYMBOLS=OFF -DCMAKE_BUILD_TYPE=Release
  fi
}

function install_stemmer {
  wget_and_untar_dependency STEMMER https://snowballstem.org/dist/libstemmer_c-${STEMMER_VERSION}.tar.gz stemmer
  (
    cd ${DEPENDENCY_DIR}/stemmer
    sed -i '/CPPFLAGS=-Iinclude/ s/$/ -fPIC/' Makefile
    make clean && make "-j${NPROC}"
    ${SUDO} cp libstemmer.a ${INSTALL_PREFIX}/lib/
    ${SUDO} cp include/libstemmer.h ${INSTALL_PREFIX}/include/
  )
}

function install_arrow {
  wget_and_untar_dependency ARROW https://archive.apache.org/dist/arrow/arrow-15.0.0/apache-arrow-15.0.0.tar.gz arrow
  cmake_install_dir arrow/cpp \
    -DARROW_PARQUET=ON \
    -DARROW_FILESYSTEM=ON \
    -DARROW_DEPENDENCY_SOURCE=AUTO \
    -DARROW_WITH_THRIFT=ON \
    -DARROW_WITH_LZ4=ON \
    -DARROW_WITH_SNAPPY=ON \
    -DARROW_WITH_ZLIB=ON \
    -DARROW_WITH_ZSTD=ON \
    -DARROW_JEMALLOC=OFF \
    -DARROW_SIMD_LEVEL=NONE \
    -DARROW_RUNTIME_SIMD_LEVEL=NONE \
    -DARROW_WITH_UTF8PROC=OFF \
    -DARROW_TESTING=ON \
    -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
    -DCMAKE_BUILD_TYPE=Release \
    -DARROW_BUILD_STATIC=ON \
    -DThrift_SOURCE=BUNDLED \
    -DCMAKE_PREFIX_PATH=${INSTALL_PREFIX} \
    -DBOOST_ROOT=${INSTALL_PREFIX}

  (
    # Install thrift.
    cd ${DEPENDENCY_DIR}/arrow/cpp/_build/thrift_ep-prefix/src/thrift_ep-build
    cmake --install ./ --prefix ${INSTALL_PREFIX}
  )
}

function install_cuda {
  # See https://developer.nvidia.com/cuda-downloads
  ${SUDO} dnf config-manager --add-repo https://developer.download.nvidia.com/compute/cuda/repos/rhel9/x86_64/cuda-rhel9.repo
  local dashed="$(echo $1 | tr '.' '-')"
  ${SUDO} dnf install -y cuda-nvcc-$dashed cuda-cudart-devel-$dashed cuda-nvrtc-devel-$dashed cuda-driver-devel-$dashed 
}

function install_velox_deps {
  run_and_time install_velox_deps_from_dnf
  run_and_time install_gflags
  run_and_time install_glog
  run_and_time install_lzo
  run_and_time install_snappy
  run_and_time install_boost
  run_and_time install_protobuf
  run_and_time install_fmt
  run_and_time install_folly
  run_and_time install_fizz
  run_and_time install_wangle
  run_and_time install_mvfst
  run_and_time install_fbthrift
  run_and_time install_duckdb
  run_and_time install_stemmer
  run_and_time install_arrow
}

(return 2> /dev/null) && return # If script was sourced, don't run commands.

(
  if [[ $# -ne 0 ]]; then
    if [[ ${USE_CLANG} != "false" ]]; then
      export CC=/usr/bin/clang-15
      export CXX=/usr/bin/clang++-15
    else
      set -u
    fi

    for cmd in "$@"; do
      run_and_time "${cmd}"
    done
    echo "All specified dependencies installed!"
  else
    if [ "${INSTALL_PREREQUISITES:-Y}" == "Y" ]; then
      echo "Installing build dependencies"
      run_and_time install_build_prerequisites
    else
      echo "Skipping installation of build dependencies since INSTALL_PREREQUISITES is not set"
    fi
    if [[ ${USE_CLANG} != "false" ]]; then
      export CC=/usr/bin/clang-15
      export CXX=/usr/bin/clang++-15
    else
      set -u
    fi
    install_velox_deps
    echo "All dependencies for Velox installed!"
    if [[ ${USE_CLANG} != "false" ]]; then
      echo "To use clang for the Velox build set the CC and CXX environment variables in your session."
      echo "  export CC=/usr/bin/clang-15"
      echo "  export CXX=/usr/bin/clang++-15"
    fi
  fi
)
