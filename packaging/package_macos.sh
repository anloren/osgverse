#!/bin/bash
# 把 build/sdk_core 打包成可双击运行的 EarthExplorer.app（修 rpath / 写 Info.plist）
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$REPO/build/sdk_core"
APP="$REPO/dist/EarthExplorer.app"
PLUGVER="osgPlugins-3.6.5"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/lib/$PLUGVER"
mkdir -p "$APP/Contents/bin"

# 1) 可执行文件
cp "$SDK/bin/osgVerse_EarthExplorer" "$APP/Contents/MacOS/"

# 2) 所有 dylib/.so（含符号链接）到 Contents/lib
cp -a "$SDK/lib/"*.dylib "$APP/Contents/lib/" 2>/dev/null || true
cp -a "$SDK/lib/"*.so "$APP/Contents/lib/" 2>/dev/null || true

# 3) 插件到 Contents/lib/osgPlugins-3.6.5；并在 Contents/bin 建同名软链（代码按 BASE_DIR/bin/osgPlugins 搜索）
cp -a "$SDK/lib/$PLUGVER/"*.so "$APP/Contents/lib/$PLUGVER/"
ln -s "../lib/$PLUGVER" "$APP/Contents/bin/$PLUGVER"

# 4) 资源目录（代码按 BASE_DIR=".." 即 Contents 下查找）
for d in shaders skyboxes textures misc models; do
    cp -a "$SDK/$d" "$APP/Contents/$d"
done

# 5) 可执行文件 rpath：删 Linux $ORIGIN，加 @executable_path/../lib
install_name_tool -delete_rpath '$ORIGIN:$ORIGIN/../lib' "$APP/Contents/MacOS/osgVerse_EarthExplorer" 2>/dev/null || true
install_name_tool -add_rpath '@executable_path/../lib' "$APP/Contents/MacOS/osgVerse_EarthExplorer"

# 6) Contents/lib 下每个 dylib/.so 加 @loader_path（同级互引用）
for f in "$APP/Contents/lib/"*.dylib "$APP/Contents/lib/"*.so; do
    [ -f "$f" ] || continue
    install_name_tool -delete_rpath '$ORIGIN:$ORIGIN/../lib' "$f" 2>/dev/null || true
    install_name_tool -delete_rpath "$SDK/lib" "$f" 2>/dev/null || true
    install_name_tool -add_rpath '@loader_path' "$f" 2>/dev/null || true
done

# 7) 插件（Contents/lib/osgPlugins-3.6.5）引用 @rpath/libosg* → 指向上一级 lib
for f in "$APP/Contents/lib/$PLUGVER/"*.so; do
    [ -f "$f" ] || continue
    install_name_tool -delete_rpath '$ORIGIN:$ORIGIN/../lib' "$f" 2>/dev/null || true
    install_name_tool -delete_rpath "$SDK/lib" "$f" 2>/dev/null || true
    install_name_tool -add_rpath '@loader_path/..' "$f" 2>/dev/null || true
done

# 8) Info.plist
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>EarthExplorer</string>
  <key>CFBundleDisplayName</key><string>osgVerse EarthExplorer</string>
  <key>CFBundleIdentifier</key><string>com.osgverse.earthexplorer</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>osgVerse_EarthExplorer</string>
  <key>NSHighResolutionCapable</key><false/>
  <key>NSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

# 9) ad-hoc 代码签名
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "[warn] codesign skipped"

echo "Built: $APP"
