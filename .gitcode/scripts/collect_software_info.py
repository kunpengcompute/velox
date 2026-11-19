#!/usr/bin/env python3
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

import os
import sys
import json
import hashlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Dict, List, Tuple, Set


def calculate_sha256(file_path: str) -> str:
    """计算文件的SHA256摘要"""
    sha256_hash = hashlib.sha256()
    try:
        with open(file_path, "rb") as f:
            # 分块读取大文件，避免内存溢出
            for chunk in iter(lambda: f.read(4096), b""):
                sha256_hash.update(chunk)
        return sha256_hash.hexdigest()
    except FileNotFoundError:
        print(f"错误：未找到软件包文件 {file_path}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"计算SHA256失败：{str(e)}", file=sys.stderr)
        sys.exit(1)


def parse_code_xml(xml_path: str) -> Dict[str, str]:
    dir_url_map = {}
    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()
        for repo in root.findall("repo"):
            dir_name = repo.get("dir")
            url = repo.get("url")
            if dir_name and url:
                dir_url_map[dir_name] = url
            else:
                print(
                    "警告：code.xml中发现无效的repo节点（dir或url为空）",
                    file=sys.stderr,
                )
    except Exception:
        pass
    return dir_url_map


def clean_git_url(url: str) -> str:
    """清理Git URL中的认证信息（账号、密码、token）"""
    if not url:
        return url

    # 匹配并移除 HTTPS URL 中的认证信息 (user:pass@ 或 user:token@)
    cleaned_url = re.sub(r"https://[^:]+:[^@]+@", "https://", url)
    return cleaned_url


def get_checked_out_tag(repo_dir: str) -> str:
    """从git reflog中解析用户实际检出的Tag（兼容git checkout tags/XX）"""
    try:
        reflog_cmd = f"git -C {repo_dir} reflog --no-abbrev -10"
        reflog_output = os.popen(reflog_cmd).read().strip()
        if not reflog_output:
            return ""

        tag_pattern = re.compile(
            r"checkout:\s+moving\s+(?:from|to)\s+.+\s+(tags/(\S+))|checkout:\s+moving\s+to\s+(\S+)$",
            re.MULTILINE,
        )
        matches = tag_pattern.findall(reflog_output)

        for match in matches:
            if match[1]:
                tag_name = match[1].strip()
                verify_cmd = f"git -C {repo_dir} show-ref --tags {tag_name}"
                if os.popen(verify_cmd).read().strip():
                    return tag_name
            elif match[2]:
                candidate_tag = match[2].strip()
                verify_cmd = f"git -C {repo_dir} show-ref --tags {candidate_tag}"
                if os.popen(verify_cmd).read().strip():
                    return candidate_tag
        return ""
    except Exception as e:
        print(f"解析检出Tag失败：{str(e)}", file=sys.stderr)
        return ""


def get_all_tags_for_commit(repo_dir: str, commit_id: str) -> Set[str]:
    """获取关联到指定Commit ID的所有Tag"""
    try:
        tag_cmd = f"git -C {repo_dir} tag --points-at {commit_id}"
        tags = os.popen(tag_cmd).read().strip().splitlines()
        return set([t.strip() for t in tags if t.strip()])
    except Exception as e:
        print(f"获取Commit {commit_id} 关联的Tag失败：{str(e)}", file=sys.stderr)
        return set()


def get_git_info(repo_dir: str) -> Tuple[str, str, str]:
    """获取git仓库信息：URL + 实际检出的Tag/所有关联Tag/分支 + Commit ID"""
    repo_path = Path(repo_dir)
    if not (repo_path / ".git").exists():
        return "", "", ""

    try:
        # 1. 获取远程仓库URL
        url_cmd = f"git -C {repo_dir} remote get-url origin"
        repo_url = os.popen(url_cmd).read().strip()

        # 清理URL中的认证信息
        repo_url = clean_git_url(repo_url)

        # 2. 获取当前Commit ID
        commit_cmd = f"git -C {repo_dir} rev-parse HEAD"
        commit_id = os.popen(commit_cmd).read().strip()
        if not commit_id:
            return repo_url, "master", ""

        # 3. 优先获取用户实际检出的Tag
        checked_out_tag = get_checked_out_tag(repo_dir)
        if checked_out_tag:
            return repo_url, checked_out_tag, commit_id

        # 4. 若未找到检出记录，返回该Commit关联的所有Tag
        all_tags = get_all_tags_for_commit(repo_dir, commit_id)
        if all_tags:
            return repo_url, ",".join(sorted(all_tags)), commit_id

        # 5. 最后获取分支
        branch_cmd = f"git -C {repo_dir} rev-parse --abbrev-ref HEAD"
        repo_branch = os.popen(branch_cmd).read().strip() or "master"

        return repo_url, repo_branch, commit_id
    except Exception as e:
        print(f"获取仓库 {repo_dir} 的Git信息失败：{str(e)}", file=sys.stderr)
        return "", "", ""


def search_repo_info(workspace: str) -> List[Dict[str, str]]:
    """在WORKSPACE目录递归查找所有包含.git的git仓库"""
    repo_info_list = []
    seen = set()  # 去重：url + ref(Tag/分支) + commit_id

    workspace_path = Path(workspace).resolve()
    if not workspace_path.exists():
        print(f"错误：WORKSPACE目录不存在 {workspace}", file=sys.stderr)
        return []

    # 递归查找所有 .git 目录，父目录即为仓库根目录
    for git_dir in workspace_path.rglob(".git"):
        repo_dir = str(git_dir.parent)
        repo_url, repo_ref, commit_id = get_git_info(repo_dir)
        if not (repo_url and commit_id):
            continue
        unique_key = (repo_url, repo_ref, commit_id)
        if unique_key not in seen:
            seen.add(unique_key)
            repo_info_list.append(
                {
                    "repoUrl": repo_url,
                    "repoBranch": repo_ref,
                    "commitId": commit_id,
                }
            )
    return repo_info_list


def main():
    # 修改入参数量：只需要 WORKSPACE + 软件包名
    if len(sys.argv) != 3:
        print(
            "用法：python3 collect_software_info.py <WORKSPACE目录> <软件包文件>",
            file=sys.stderr,
        )
        print(
            "示例：python3 collect_software_info.py /workspace mypackage.tar.gz",
            file=sys.stderr,
        )
        sys.exit(1)

    workspace = sys.argv[1]
    package_name = sys.argv[2]
    package_path = Path(package_name)

    # 1. 计算软件包SHA256
    sha256sum = calculate_sha256(str(package_path))

    # 2. 递归扫描workspace收集所有git仓库信息
    repo_info = search_repo_info(workspace)

    # 3. 获取构建时间（格式化为纯数字）
    from datetime import datetime

    # 格式说明：%Y(年)%m(月)%d(日)%H(时)%M(分)%S(秒) → 例如 20260304153022
    build_time = datetime.now().strftime("%Y%m%d%H%M%S")

    # 4. 构建JSON数据，保持原有JSON结构不变
    json_data = {
        "sha256Sum": sha256sum,
        "repoInfo": repo_info,
        "buildTime": build_time,
    }

    # 5. 写入JSON文件
    json_filename = f"{package_name}.json"
    try:
        with open(json_filename, "w", encoding="utf-8") as f:
            json.dump(json_data, f, ensure_ascii=False, indent=4)
        print(f"成功生成JSON文件：{json_filename}")
    except Exception as e:
        print(f"写入JSON文件失败：{str(e)}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
