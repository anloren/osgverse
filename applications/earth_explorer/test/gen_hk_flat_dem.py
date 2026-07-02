#!/usr/bin/env python3
"""生成香港城区"羽化压平"高程瓦片(修 Terrarium DSM 假土包,且无 LOD/邻接落差)。

背景:AWS Terrarium(SRTM 派生)在高楼区把楼高掺进地形(尖沙咀 p90≈37m,真实地面
3-8m),×TileElevationScale=2 后城区变 80-120m 假土包。直接对 bbox 内瓦片返回平坦
几何会在 bbox 边界/父子 LOD 之间产生百米落差 →"漏空的壳"(与滇池看穿孔洞同族)。

本脚本的做法:对 z8-15 所有与影响区相交的瓦片,逐像素应用同一个连续函数
    h' = w(lat,lon) * h + (1 - w) * min(h, FLAT_H)
其中 w 在核心区=0(压平到不超过 FLAT_H),向外经 FEATHER 宽的渐变带平滑升到 1
(完全保留原始高程)。所有层级用同一函数处理 → 父子 LOD、bbox 内外邻接由构造保证
一致,无缝。z>15 沿用引擎"z15 祖先一步烘焙"机制,自动吃到本地 z15 瓦片。

输出:assets/models/Earth/hk_dem/{z}/{x}/{yXYZ}.png(仅保存被明显修改的瓦片);
运行时 earth_main.cpp 的 createCustomPath 发现本地文件存在即用之替代 AWS 瓦片。

用法:python3 gen_hk_flat_dem.py   (幂等,重跑覆盖)
"""
import io
import math
import os
import sys
import urllib.request

import numpy as np
from PIL import Image

# 核心压平区(九龙半岛 + 港岛北岸平原;刻意避开太平山/狮子山/魔鬼山等真山体)
LAT0, LAT1 = 22.276, 22.335
LON0, LON1 = 114.115, 114.225
FEATHER = 0.012          # 渐变带宽度(度,~1.2km)
FLAT_H = 6.0             # 核心区地面高度上限(米)
ZMIN, ZMAX = 8, 15       # 处理层级(z<8 时假土包只占子像素,可忽略;z>15 走 z15 祖先)
CHANGE_EPS = 1.0         # 整瓦片最大改动 < 1m 则不保存(视为未受影响)

URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "../../../assets/models/Earth/hk_dem")


def tile_lon(x, z):
    return x / (2.0 ** z) * 360.0 - 180.0


def tile_lat(y, z):
    return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / (2.0 ** z)))))


def lat_to_yf(lat, z):
    r = math.radians(lat)
    return (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * (2.0 ** z)


def lon_to_xf(lon, z):
    return (lon + 180.0) / 360.0 * (2.0 ** z)


def weight(lat, lon):
    """0=核心区(压平) → 1=渐变带外(原样)。按到核心矩形的外距离线性升,再 smoothstep。"""
    dlat = max(LAT0 - lat, lat - LAT1, 0.0)
    dlon = max(LON0 - lon, lon - LON1, 0.0)
    d = math.hypot(dlat, dlon)
    t = min(d / FEATHER, 1.0)
    return t * t * (3 - 2 * t)


def process_tile(z, x, y):
    req = urllib.request.Request(URL.format(z=z, x=x, y=y),
                                 headers={"User-Agent": "osgverse-hk-dem-prep"})
    img = None
    for attempt in range(4):   # 瞬时网络错误(SSL EOF 等)重试
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                img = Image.open(io.BytesIO(r.read())).convert("RGB")
            break
        except Exception:
            if attempt == 3:
                raise
    a = np.asarray(img, dtype=np.float64)
    h = a[:, :, 0] * 256.0 + a[:, :, 1] + a[:, :, 2] / 256.0 - 32768.0

    n = 256
    lats = np.array([tile_lat(y + (i + 0.5) / n, z) for i in range(n)])
    lons = np.array([tile_lon(x + (j + 0.5) / n, z) for j in range(n)])
    w = np.empty((n, n))
    for i in range(n):
        for j in range(n):
            w[i, j] = weight(lats[i], lons[j])

    h2 = w * h + (1.0 - w) * np.minimum(h, FLAT_H)
    if np.abs(h2 - h).max() < CHANGE_EPS:
        return False

    v = h2 + 32768.0
    r8 = np.floor(v / 256.0)
    g8 = np.floor(v - r8 * 256.0)
    b8 = np.floor((v - np.floor(v)) * 256.0)
    out = np.stack([r8, g8, b8], axis=2).clip(0, 255).astype(np.uint8)
    path = os.path.join(OUT, str(z), str(x))
    os.makedirs(path, exist_ok=True)
    Image.fromarray(out).save(os.path.join(path, "%d.png" % y), optimize=True)
    return True


def main():
    total = 0
    for z in range(ZMIN, ZMAX + 1):
        x0 = int(lon_to_xf(LON0 - FEATHER, z)); x1 = int(lon_to_xf(LON1 + FEATHER, z))
        y0 = int(lat_to_yf(LAT1 + FEATHER, z)); y1 = int(lat_to_yf(LAT0 - FEATHER, z))
        saved = 0
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                try:
                    if process_tile(z, x, y):
                        saved += 1
                except Exception as e:
                    print("  !! z%d %d/%d: %s" % (z, x, y, e)); sys.exit(1)
        total += saved
        print("z%-2d: %d tiles saved (scanned %dx%d)"
              % (z, saved, x1 - x0 + 1, y1 - y0 + 1))
    print("done, %d tiles -> %s" % (total, os.path.normpath(OUT)))


if __name__ == "__main__":
    main()
