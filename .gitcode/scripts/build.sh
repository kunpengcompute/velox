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
readonly NUM_THREADS=${NUM_THREADS:-8}

usage() {
  echo "usage: $0 {compile|ut|package}" >&2
  exit 2
}

function prepare() {
  echo "install common dependencies"
}

function build_velox() {
  echo "请编写编译脚本"
}

function ut_velox() {
  echo "请编写ut执行脚本"
}

function BoostKit_velox_Version_info() {
  touch version.txt
  echo "Product Name: Kunpeng BoostKit" >>version.txt
  echo "Product Version: ${Product_Version}" >>version.txt
  echo "Component Name: BoostKit-velox" >>version.txt
  echo "Component Version: ${velox_Version}" >>version.txt
}

function package_velox() {
  echo "请编写打包脚本"
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
  : "${Product_Version:?版本构建需由工作流传入 Product_Version}"
  : "${velox_Version:?请在 build.ini 的 [build] 段配置 velox_Version}"
  : "${module:?版本构建需由工作流注入 module}"
  prepare
  build_velox
  package_velox
  ;;
*)
  usage
  ;;
esac
