#!/usr/bin/env python3
"""
发布 ESP32 固件到下载服务器。

用法：
    python publish.py                    # 使用 build/NiceAI.bin

工作流程：
    1. idf.py build
    2. python publish.py
    3. ESP32 重启连接服务器 → 收到新版本号 → OTA 自动升级

版本号直接从固件二进制中读取（PROJECT_VER），确保版本一致。
"""
import os
import sys
import json
import shutil
import re
import subprocess

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
DEST_DIR = "/root/nice_ai/dev-nice-ai_cn/public/download/esp32s3"
FIRMWARE_URL = "https://dev.nice-ai.cn/download/esp32s3/NiceAI.bin"
CMAKE_FILE = os.path.join(PROJECT_ROOT, "CMakeLists.txt")


def read_cmake_project_ver() -> str:
    with open(CMAKE_FILE) as f:
        content = f.read()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', content)
    return m.group(1) if m else "unknown"


def read_bin_project_ver(firmware_path: str) -> str:
    """从固件二进制中读取 PROJECT_VER"""
    try:
        result = subprocess.run(
            ["strings", firmware_path],
            capture_output=True,
            text=True,
            timeout=5,
        )
        for line in result.stdout.splitlines():
            line = line.strip()
            if re.match(r"^\d{8}\.\d{4}$", line):
                return line
    except Exception as e:
        print(f"[警告] 读取固件版本失败: {e}")
    return ""


def publish(firmware_path: str):
    if not os.path.exists(firmware_path):
        print(f"[错误] 固件不存在: {firmware_path}")
        print(f"     请先执行: idf.py build")
        sys.exit(1)

    os.makedirs(DEST_DIR, exist_ok=True)

    # 从固件中读取版本号
    bin_version = read_bin_project_ver(firmware_path)
    cmake_version = read_cmake_project_ver()

    if not bin_version:
        print(f"[错误] 无法从固件中读取版本号")
        print(f"     请确保已执行 idf.py build")
        sys.exit(1)

    print(f"[版本] 固件中: {bin_version}")
    print(f"[版本] CMake:  {cmake_version}")

    # 如果 CMake 版本与固件版本不一致，更新 CMake
    if cmake_version != bin_version:
        print(f"[更新] CMakeLists.txt PROJECT_VER: {cmake_version} → {bin_version}")
        with open(CMAKE_FILE) as f:
            content = f.read()
        content = re.sub(
            r'set\(PROJECT_VER\s+"[^"]*"\)',
            f'set(PROJECT_VER "{bin_version}")',
            content,
        )
        with open(CMAKE_FILE, "w") as f:
            f.write(content)

    # 复制固件
    dest_bin = os.path.join(DEST_DIR, "NiceAI.bin")
    shutil.copy2(firmware_path, dest_bin)
    fsize = os.path.getsize(dest_bin)
    print(f"[OK] 已复制: {dest_bin} ({fsize / 1024 / 1024:.1f} MB)")

    # 更新 version.json
    version_json = {
        "version": bin_version,
        "version_name": bin_version,
        "download_url": FIRMWARE_URL,
        "release_notes": f"ESP32-S3 固件 {bin_version}",
        "firmware": {
            "version": bin_version,
            "url": FIRMWARE_URL,
        },
    }
    dest_json = os.path.join(DEST_DIR, "version.json")
    with open(dest_json, "w") as f:
        json.dump(version_json, f, indent=2, ensure_ascii=False)
    print(f"[OK] 已更新: {dest_json}")
    print(f"[OK] 下载页面: https://dev.nice-ai.cn/download/?app=esp32s3")
    print(f"[OK] OTA 地址: https://dev.nice-ai.cn/download/esp32s3/version.json")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        firmware_path = sys.argv[1]
    else:
        firmware_path = os.path.join(PROJECT_ROOT, "build", "NiceAI.bin")

    publish(firmware_path)