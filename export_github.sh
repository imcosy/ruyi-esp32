#!/usr/bin/env bash
# =============================================================
# ruyi-esp32 公开仓库发布脚本
# 用法：bash export_github.sh "提交说明"
#
# 原则：
#   - 本目录（/root/nice_ai/sisi_esp）是开发主目录，私有仓库 nice_ai 照常管理；
#   - 公开仓库只发布"可开源"的内容：内部调试探针、密钥、需求文档一律不出去；
#   - 本地 sdkconfig.* 含真实 api_key，导出时统一清空，公开仓库用空 key 默认值。
# =============================================================
set -euo pipefail

SRC=/root/nice_ai/sisi_esp
DST=/root/.publish/ruyi-esp32
REMOTE=git@github.com:imcosy/ruyi-esp32.git
MSG="${1:-sync from dev}"

# ① 同步（--delete 保持与主目录一致；排除内部/含密钥/构建产物）
mkdir -p "$DST"
rsync -a --delete \
  --exclude='.cache/' \
  --exclude='build/' \
  --exclude='managed_components/' \
  --exclude='meeting/' \
  --exclude='scripts/' \
  --exclude='.git/' \
  --exclude='sdkconfig' \
  --exclude='sdkconfig.old' \
  --exclude='sdkconfig.anan' \
  --exclude='server/config.yaml' \
  --exclude='server/__pycache__/' \
  --exclude='**/__pycache__/' \
  --exclude='火种计划S2_参赛方案.md' \
  --exclude='需求文档.md' \
  --exclude='log.md' \
  "$SRC/" "$DST/"

# ② 清洗板型配置里的密钥（sisi/meeting 板型配置开源保留，key 置空）
sed -i 's/CONFIG_S2S_API_KEY="[^"]*"/CONFIG_S2S_API_KEY=""/' "$DST"/sdkconfig.* 2>/dev/null || true

# ③ 终检：公开仓库不允许出现任何真实密钥
#    兼容带连字符的密钥（sk-sisi-esp-xxxx 这类不会被 sk-[a-zA-Z0-9]{20,} 命中）
if grep -rIlnE 'sk-[A-Za-z0-9][A-Za-z0-9_-]{15,}' "$DST" --exclude-dir=.git 2>/dev/null ; then
  echo "!! 发现疑似密钥，中止发布"; exit 1
fi

# ④ 提交推送（ruyi-esp32 专用 deploy key：~/.ssh/id_ruyi）
cd "$DST"
if [ ! -d .git ]; then
  git init -b main
  git remote add origin "$REMOTE"
fi
git add -A
if git diff --cached --quiet; then
  echo "无变更"
else
  git commit -m "$MSG"
fi
GIT_SSH_COMMAND="ssh -i $HOME/.ssh/id_ruyi -o IdentitiesOnly=yes" git push -u origin main
echo "已发布到 github.com/imcosy/ruyi-esp32"
