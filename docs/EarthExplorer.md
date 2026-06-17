# EarthExplorer 使用指南

EarthExplorer 是一个基于 osgVerse 的高精度流式三维地球应用。它在运行时从互联网实时拉取 ArcGIS World Imagery 卫星正射影像瓦片与 AWS Terrain Tiles 高程数据（Terrarium 编码），在 OpenGL 4.1 Core Profile 下渲染出带地形起伏的高清地球，并提供 ImGui 悬浮控制面板用于调节视角、光照、海洋和曝光参数，以及书签巡游功能。

## 命令行运行

> 需要联网，程序启动后实时加载在线瓦片。

```bash
cd /Users/franklee/osgverse
packaging/run.sh
```

`run.sh` 会自动设置 `DYLD_LIBRARY_PATH` 并启动 `build/sdk_core/bin/osgVerse_EarthExplorer`。

数据源：

https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}

https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png

## 桌面应用

首先用打包脚本生成 `.app` 包：

```bash
cd /Users/franklee/osgverse
packaging/package_macos.sh
```

完成后双击打开，或用命令行：

```bash
open /Users/franklee/osgverse/dist/EarthExplorer.app
```

> 注意：`dist/` 目录已加入 `.gitignore`，每次克隆后需重新运行打包脚本。

## 控制面板

应用启动后，左上角浮现 **Earth Control / 地球控制台** 面板，功能如下：

- **相机读数**：实时显示当前视点的纬度 (Lat)、经度 (Lon)、高度 (km)。
- **太阳方位 / 高度**：滑动条调节太阳方位角 (Az) 与仰角 (El)，控制光照方向和阴影。
- **渲染选项**：海洋 (Ocean) 开关、曝光系数 (Exposure) 滑动条。
- **跳转经纬度 (Go To)**：输入目标 Lat / Lon，点击 Go 立即飞向该位置。
- **书签巡游 (Bookmarks)**：预存若干地标，支持单步跳转或自动巡游。

## 鼠标操作

| 操作 | 效果 |
|------|------|
| 左键拖动 | 旋转地球 / 改变观察角度 |
| 右键拖动 | 俯仰视角 |
| 滚轮向前 | 拉近（自动加载更高分辨率在线瓦片） |
| 滚轮向后 | 拉远（切换到低级别瓦片） |
