# osgVerse macOS 编译经验（DEFAULT / OpenGL Compatible, arm64）

环境：macOS Darwin 25, Apple Silicon (arm64), Xcode CLT clang 21, cmake 4.3, OSG 3.6.5。
工作目录：/Users/franklee/osgverse，OSG 源码预克隆到 /Users/franklee/OpenSceneGraph（Setup.sh 期望同级 ../OpenSceneGraph）。

## 修复 1：libpng 误包含经典 Mac OS `<fp.h>`
- 现象：编译 3rdparty 时 `helpers/toolchain_builder/png/pngpriv.h:527 fatal error: 'fp.h' file not found`。
- 根因：现代 macOS 的 TargetConditionals.h 定义 `TARGET_OS_MAC=1`，触发了为 Classic Mac OS 写的 `<fp.h>` 包含分支。
- 修复：pngpriv.h ~520 行的条件里删除 `|| defined(TARGET_OS_MAC)`，使其落到 `#else` 的 `<math.h>`。

## 修复 2：OSG tiff 插件链接缺 libjpeg 符号
- 现象：链接 `osgdb_tiff.so` 报一堆 `_jpeg_*` undefined symbols for arm64。
- 根因：捆绑 libtiff 启用 JPEG（tif_jpeg.c），但 OSG tiff 插件只链接 TIFF_LIBRARY，未带 libjpeg；静态库不传递依赖。
- 修复：`../OpenSceneGraph/src/osgPlugins/tiff/CMakeLists.txt` 把 `SET(TARGET_LIBRARIES_VARS TIFF_LIBRARY)` 改为 `... TIFF_LIBRARY JPEG_LIBRARY)`。JPEG_LIBRARY 已由 Setup.sh 传入 OSG cmake 缓存，无需额外定义。

## 修复 3：OSG freetype 插件链接缺 libpng/zlib 符号
- 现象：链接 `osgdb_freetype.so` 报 `_png_*` undefined（freetype 的 sbit PNG 字形支持 `Load_SBit_Png` 依赖 libpng，libpng 又依赖 zlib）。
- 修复：`../OpenSceneGraph/src/osgPlugins/freetype/CMakeLists.txt` 的 `SET(TARGET_EXTERNAL_LIBRARIES ${FREETYPE_LIBRARIES})` 改为追加 `${PNG_LIBRARY_RELEASE} ${ZLIB_LIBRARY_RELEASE}`。
- 关键坑：不能用 `${PNG_LIBRARY}`（OSG FindPNG 未填充该名字，展开为空）；必须用 cmake 缓存里确有值的 `PNG_LIBRARY_RELEASE`。zlib 用 `${ZLIB_LIBRARIES}` 或 `${ZLIB_LIBRARY_RELEASE}` 均可。

## 修复 4：GLFW（Cocoa 后端）链接缺 macOS 系统框架
- 现象：编译 osgVerse 阶段 `osgVerse_PreTest_GL` 链接报 `_CFArrayAppendValue` 等 undefined（来自 libglfw.a 的 cocoa_*.m）。
- 根因：`helpers/toolchain_builder/GLFW/CMakeLists.txt` 的 glfw 静态库未链接 Cocoa/IOKit/CoreFoundation/CoreVideo 框架。
- 修复：在 `ADD_LIBRARY(glfw STATIC ...)` 后加 `IF(APPLE) TARGET_LINK_LIBRARIES(glfw "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo") ENDIF()`。静态库的 TARGET_LINK_LIBRARIES 作为 INTERFACE 传播给所有使用者（GLFW_test、readerwriter），一次修复。

## 修复 5：AppleClang 把 -Wnon-pod-varargs 当错误
- 现象：编译 `wrappers/generic_osg/FragmentProgram.cpp:26` 报 `cannot pass object of non-trivial type 'osg::Matrixd' through variadic method`（osgVerse 序列化器 `InputUserData::add(...)` 大量这样传 Vec4d/Matrixd）。
- 根因：osg::Matrixd/Vec4d 平凡可复制、arm64 varargs 实际可用，但 AppleClang 默认把该警告升级为错误。
- 修复：根 `CMakeLists.txt` 的 `IF(NOT WIN32) ADD_COMPILE_OPTIONS(-Wno-psabi)` 之后加 `IF(APPLE) ADD_COMPILE_OPTIONS(-Wno-non-pod-varargs) ENDIF()`，全局降级，避免逐个改 wrapper。
- 注意：该改动是全局编译选项，cmake 重配后会触发 osgVerse 全量重编。

## 加速：避免每次跑完整 Setup.sh
- 续编 OSG：直接 `cd build/osg_def && cmake . && cmake --build . --target install`（增量，仅重链改动插件，~秒级到达失败点）。
- OSG 装好后（build/sdk/bin/osgviewer 存在），再跑 `Setup.sh DEFAULT` 会自动 SkipOsgBuild=1，跳过 3rdparty/OSG，直接编译 osgVerse。

## 关键：macOS 桌面必须用 CORE 模式，不能用 DEFAULT
- 现象：DEFAULT（OpenGL Compatible）构建的 EarthExplorer 能启动，但着色器全线失败 `version '130' is not supported`；运行日志显示上下文是 `OpenGL 2.1 Metal; GLSL: 120`。
- 根因：现代 macOS 原生 OpenGL 在兼容档(compatibility)只给 GL2.1/GLSL120；osgVerse 着色器要 `#version 130+`（`Pipeline.cpp:1459` 强制 ≥130）。只有 **Core Profile** 才能拿到 GL3.3+/GLSL150+。
- 机制：`pipeline/ShaderLibrary.cpp:232` 在定义了 `OSG_GL3_AVAILABLE`（CORE 编译的 OSG）时发出 `#version 330 core`，macOS 支持。
- 解决：用 `Setup.sh CORE` 构建（sdk_core/verse_core）。之前所有源码编译修复都在源码树，自动复用。结果在 `build/sdk_core/bin/`，运行需 `DYLD_LIBRARY_PATH=build/sdk_core/lib`。

## 修复 6：verse_tiff 插件未构建（mbtiles .tif 高程/遮罩解码失败）
- 现象：运行日志 `[mbtiles] No reader/writer plugin for tif`；`build/sdk/lib/osgPlugins-*/` 里没有 `osgdb_verse_tiff.so`。
- 根因：插件被 `IF(VERSE_WITH_TIFF_LIBRARY)` 门控；根 CMakeLists line 732 用 `IF(TIFF_FOUND)` 置位，但 Setup.sh 只传 `TIFF_INCLUDE_DIR/TIFF_LIBRARY_RELEASE`、从不跑 `find_package(TIFF)`，故 `TIFF_FOUND` 为空。
- 修复：根 CMakeLists 改为 `IF(TIFF_FOUND OR TIFF_LIBRARY_RELEASE) SET(TIFF_FOUND ON) SET(VERSE_WITH_TIFF_LIBRARY ON) ...`。

## 截图：用 OSG ScreenCaptureHandler，不要 OS screencapture
- macOS `screencapture` 报 `could not create image from display`（缺屏幕录制 TCC 权限，无法编程授予）。
- 改用 OSG `osgViewer::ScreenCaptureHandler`（glReadPixels 读 framebuffer）截图，绕过权限。

## 运行注意
- 后台启动构建务必先 `cd /Users/franklee/osgverse`（Bash 后台 shell cwd 可能被重置到 claudecode，导致 exit 127）。
- 重新运行 Setup.sh DEFAULT 是增量的：3rdparty/OSG 已缓存在 build/ 下，只重配+重链+继续。
