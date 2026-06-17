#!/bin/sh
# 命令行启动 EarthExplorer（CORE 构建产物，设好 DYLD 与工作目录）
SDK="$(cd "$(dirname "$0")/../build/sdk_core" && pwd)"
cd "$SDK/bin" || exit 1
DYLD_LIBRARY_PATH="$SDK/lib" exec ./osgVerse_EarthExplorer "$@"
