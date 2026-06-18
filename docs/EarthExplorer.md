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

## 瓦片缓存

首次访问的瓦片下载后自动缓存到本地磁盘：

```
~/.osgverse_earth_cache/
```

再次打开相同区域时，程序直接读取缓存，启动更快，断网时也能浏览已访问的区域。

如需更改缓存目录，设置环境变量 `EARTH_TILE_CACHE`：

```bash
EARTH_TILE_CACHE=/Volumes/SSD/earth_cache packaging/run.sh
```

## 真实时间太阳

面板 **太阳 Sun** 区段中有 "真实时间太阳 Real-time" 复选框。

- **勾选后**：程序读取系统时钟（UTC），按 NOAA 日面算法实时计算太阳方向，每帧更新光照，可观察到明暗分界线（晨昏线）随时间移动。
- **取消勾选**：切回手动模式，用 Azimuth / Elevation 滑条自由设置光照角度。
- **手动指定 UTC 时间**：Real-time 勾选状态下，面板会显示 UTC 日期时间输入框，填入后即锁定到该时刻的太阳位置，方便演示特定时刻的光照效果（如日出/日落）。

## 大气 / 曝光

面板 **渲染 Render** 区段提供两个滑块：

- **曝光 Exposure**（默认 0.25）：整体亮度系数，降低可抑制过曝，升高可增亮暗侧。
- **大气强度 Atmosphere**（默认 1.00）：控制大气散射的视觉强度；调至 0 可关闭大气晕；调至 2 可增强蓝色大气辉光。

## 相机限制

滚轮拉近时相机距地表最低约 **50 米**，无法穿入地下。接近地表时滚轮速度自动减慢以便精细操控。
