#!/usr/bin/env python3
"""启动脚本：python run.py（配置读 config.yaml 的 server.host/port）"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uvicorn

from s2s.config import load_config


def main():
    cfg = load_config()["server"]
    uvicorn.run("s2s.app:app", host=cfg["host"], port=int(cfg["port"]), log_level="info")


if __name__ == "__main__":
    main()
