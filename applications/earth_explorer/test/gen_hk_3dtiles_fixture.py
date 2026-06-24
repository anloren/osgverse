#!/usr/bin/env python3
"""Generate a deterministic, georeferenced Cesium 3D Tiles fixture for the
EarthExplorer 3D Tiles overlay (the EARTH_3DTILES hook in tiles3d_data.cpp).

Why this exists: public sample tilesets we tried (earthsdk dayanta, ArcGIS
Stuttgart) returned 404, so this synthesizes a controllable local fixture.
It bakes a WGS84 ENU->ECEF local frame at a given lat/lon into the tileset
root transform, so a Y-up glb stands at that geographic location. Used to
verify that osgVerse's osgdb_3dtiles plugin reads the root transform
correctly and positions content at the right place on the globe.

Usage:
  python3 gen_hk_3dtiles_fixture.py [lat] [lon] [scale] [outdir]
  # defaults: 22.2816 114.1578 (HK Central)  scale=100  outdir=/tmp/hk_fixture

Then run EarthExplorer (from build/sdk_core/bin):
  EARTH_3DTILES=<outdir>/tileset.json ./osgVerse_EarthExplorer --goto <lat> <lon> 5
The log line "[Tiles3D] loaded: ... bound center=<x y z>" should match the
printed expected ECEF coordinates (within scale * model-radius).
"""
import math, json, os, sys, shutil

lat_d  = float(sys.argv[1]) if len(sys.argv) > 1 else 22.2816
lon_d  = float(sys.argv[2]) if len(sys.argv) > 2 else 114.1578
s      = float(sys.argv[3]) if len(sys.argv) > 3 else 100.0
outdir = sys.argv[4] if len(sys.argv) > 4 else "/tmp/hk_fixture"

a = 6378137.0; f = 1.0/298.257223563; e2 = f*(2.0-f)
lat = math.radians(lat_d); lon = math.radians(lon_d); h = 0.0
slat, clat = math.sin(lat), math.cos(lat)
slon, clon = math.sin(lon), math.cos(lon)
N = a/math.sqrt(1.0 - e2*slat*slat)
ox = (N+h)*clat*clon
oy = (N+h)*clat*slon
oz = (N*(1.0-e2)+h)*slat
east  = (-slon,       clon,       0.0)
north = (-slat*clon, -slat*slon,  clat)
up    = ( clat*clon,  clat*slon,  slat)

# array order == plugin's osg::Matrix(m) (row-major, v*M) == Cesium column-major.
# columns: x->east, y->up (glb Y-up -> local up), z->north.
T = [ s*east[0],  s*east[1],  s*east[2],  0.0,
      s*up[0],    s*up[1],    s*up[2],    0.0,
      s*north[0], s*north[1], s*north[2], 0.0,
      ox, oy, oz, 1.0 ]

tileset = {
  "asset": {"version": "1.0"},
  "geometricError": 1000,
  "root": {
    "transform": T,
    "boundingVolume": {"box": [0,0,0, 300,0,0, 0,300,0, 0,0,300]},
    "geometricError": 0, "refine": "ADD",
    "content": {"uri": "model.glb"}
  }
}

os.makedirs(outdir, exist_ok=True)
json.dump(tileset, open(os.path.join(outdir, "tileset.json"), "w"), indent=2)

# copy a bundled glb as the tile content
repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
src_glb = os.path.join(repo, "assets", "models", "Characters", "girl.glb")
if os.path.exists(src_glb):
    shutil.copy(src_glb, os.path.join(outdir, "model.glb"))
else:
    print("WARN: bundled glb not found at", src_glb,
          "- copy any .glb to", outdir + "/model.glb")

print("wrote", os.path.join(outdir, "tileset.json"))
print("expected ECEF bound center ~= (%.0f, %.0f, %.0f)" % (ox, oy, oz))
print("run: EARTH_3DTILES=%s/tileset.json ./osgVerse_EarthExplorer --goto %s %s 5"
      % (outdir, lat_d, lon_d))
