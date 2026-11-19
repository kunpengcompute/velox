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

# 统一配置加载器：供门禁 / 版本构建两类场景共用。
# 取值优先级：CI 显式入参(环境变量) > 场景 section > [build] 默认值 > 脚本内置兜底。
# 门禁(PR)无法手动入参，因此环境变量为空时自然回落到 build.ini 默认值。

set -e

SCRIPT_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_ROOT}/../" && pwd)
INI_FILE="${INI_FILE:-${REPO_ROOT}/build.ini}"

# 需要在两类场景间统一解析的配置项；module 由工作流直接注入，不参与本表
CONFIG_KEYS="
velox_Version
"

get_ini() {
  local section="$1"
  local key="$2"
  [[ -f ${INI_FILE} ]] || return 0
  awk -v sec="${section}" -v k="${key}" '
    /^\[.*\]/ { in_sec=($0 == "[" sec "]") }
    in_sec && match($0, "^[[:space:]]*" k "[[:space:]]*=[[:space:]]*(.*)", arr) {
      gsub(/[[:space:]]+$/, "", arr[1])
      gsub(/^["\047]|["\047]$/, "", arr[1])
      print arr[1]
      exit
    }
  ' "${INI_FILE}"
}

load_config() {
  local key value
  for key in ${CONFIG_KEYS}; do
    value="${!key:-}"
    [[ -n ${value} ]] || value="$(get_ini "${BUILD_SCENARIO:-build}" "${key}")"
    [[ -n ${value} ]] || value="$(get_ini build "${key}")"
    export "${key}=${value}"
  done
}

load_config

set_ccache() {
  mkdir -p /home/jenkins/agent/build_cache/.ccache
  sudo chown -R jenkins:jenkins /home/jenkins/agent/build_cache || true
  sudo chmod -R 755 /home/jenkins/agent/build_cache || true
  command -v ccache >/dev/null 2>&1 || return 0
  export CCACHE_DIR="/home/jenkins/agent/build_cache/.ccache"
  mkdir -p "${CCACHE_DIR}"
  ccache -M 0 >/dev/null 2>&1 || true
  export CCACHE_COMPRESS=true
  export CCACHE_HASHDIR=true
  export PATH="${CCACHE_DIR}/bin:$PATH"
  export PATH="/usr/lib64/ccache:$PATH"
}
