#include <algorithm>
#include <osg/io_utils>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/ReadFile>

#include <pipeline/Utilities.h>
#include <readerwriter/Utilities.h>
#include <readerwriter/TileCallback.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <atomic>
#include <cstdlib>

namespace
{
    // Persistent worker pool used to load a single tile's independent layers
    // (elevation/ortho/mask/labels/overlay) concurrently. A tile created at deep zoom
    // sits on the drill-down critical path (one tile per level, each waiting on its
    // parent), and its layers are independent network/cache fetches — loading them in
    // parallel cuts the per-tile wall time roughly to the slowest single layer.
    //
    // Why a *persistent* pool and not just std::thread/std::async per layer: loadFileData
    // keeps a thread_local keep-alive HTTP client per host, so a fresh TLS handshake
    // (~0.6s, more than half a tile's cost) is paid only once per worker, not per fetch.
    // Ephemeral threads would re-handshake every time and be slower than the old
    // sequential path. Long-lived workers accumulate and reuse their connections.
    //
    // Size from EARTH_TILE_POOL (default 8). 0 disables the pool entirely: runAll() then
    // runs every task inline on the calling pager thread — byte-for-byte the old behavior.
    class LayerLoadPool
    {
    public:
        static LayerLoadPool& instance()
        {
            // Intentionally leaked: workers are detached and may run until process exit;
            // never destroying the pool avoids a use-after-free of the queue at teardown.
            static LayerLoadPool* p = new LayerLoadPool(poolSize());
            return *p;
        }

        static int poolSize()
        {
            static int n = []() {
                const char* e = getenv("EARTH_TILE_POOL");
                int v = (e && e[0]) ? atoi(e) : 8; return v < 0 ? 0 : v;
            }();
            return n;
        }

        // Run all tasks, blocking until every one finishes. With the pool enabled, all but
        // one task are dispatched to workers and the last runs inline on the caller (so the
        // pager thread's own keep-alive connection does useful work instead of idling).
        void runAll(std::vector<std::function<void()> >& tasks)
        {
            size_t n = tasks.size();
            if (n == 0) return;
            if (_workers == 0 || n == 1) { for (size_t i = 0; i < n; ++i) tasks[i](); return; }

            std::shared_ptr<Latch> latch(new Latch((int)(n - 1)));
            {
                std::lock_guard<std::mutex> lk(_qm);
                for (size_t i = 1; i < n; ++i)
                {
                    std::function<void()> fn = tasks[i];
                    _q.push([fn, latch]() { fn(); latch->countDown(); });
                }
            }
            _cv.notify_all();
            tasks[0]();      // run one layer inline on this pager thread
            latch->wait();   // then wait for the dispatched layers
        }

    private:
        struct Latch
        {
            std::mutex m; std::condition_variable cv; int count;
            Latch(int c) : count(c) {}
            void countDown() { std::unique_lock<std::mutex> lk(m); if (--count <= 0) cv.notify_all(); }
            void wait() { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [this]() { return count <= 0; }); }
        };

        explicit LayerLoadPool(int workers) : _workers(workers)
        {
            for (int i = 0; i < _workers; ++i)
                std::thread([this]() { workerLoop(); }).detach();
        }

        void workerLoop()
        {
            for (;;)
            {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(_qm);
                    _cv.wait(lk, [this]() { return !_q.empty(); });
                    job = std::move(_q.front()); _q.pop();
                }
                job();
            }
        }

        int _workers;
        std::mutex _qm;
        std::condition_variable _cv;
        std::queue<std::function<void()> > _q;
    };
}

// WebMercatorTiles: (0-0-0) Lv1 = 00,01,10,11, ...; (0-0-x) Lv0 = 00,01,10,11, ...
// WGS84Tiles: (0-0-0) Lv1 = 00,10, Lv2 = 00,01,10,11,20,21,30,31, ...; (0-0-x) Lv0 = 00,10, ...

// osgviewer 0-0-0.verse_tms -O "Orthophoto=https://xxxx.com/tile/{z}/{y}/{x} ..."
// osgviewer 0-0-x.verse_tms -O "Orthophoto=E:\\testTMS\\{z}\\{x}\\{y}.png ..."
// osgviewer 0-0-x.verse_tms -O "Orthophoto=E:\\testTMS\\all_in_pack.mbtiles ..."

/* Tile map servers:
     Arcgis Map:
     - Satellite: https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}
     - Roadmap: https://server.arcgisonline.com/arcgis/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}
     - Terrain: https://server.arcgisonline.com/arcgis/rest/services/WorldElevation3D/Terrain3D/ImageServer/tile/{z}/{y}/{x}
       - ArcGIS Terrain 3D format (application/octet-stream, gzip)
       - Info: https://server.arcgisonline.com/arcgis/rest/services/WorldElevation3D/Terrain3D/ImageServer
     GapDe Map: https://webst01.is.autonavi.com/appmaptile?style%3d{s}&x%3d{x}&y%3d{y}&z%3d{z}
     - {s}: 6 = satellite, 7 = roadmap
     TengXun Map: (https://blog.csdn.net/mygisforum/article/details/22997879)
     - http://p0.map.gtimg.com/demTiles/{z}/{x/16}/{y/16}/{x}_{y}.jpg
     - http://p0.map.gtimg.com/sateTiles/{z}/{x/16}/{y/16}/{x}_{y}.jpg
     - options->setPluginData("UrlPathFunction", (void*)createTengXunPath);
     - Custom function:
       static std::string createTengXunPath(int type, const std::string& prefix, int x, int y, int z) {
           std::string path = prefix; bool changed = false;
           path = replace(path, "{x16}", std::to_string(x / 16), changed);
           path = replace(path, "{y16}", std::to_string(y / 16), changed);
           y = (int)pow((double)z, 2.0) - y - 1; }
     Baidu Map: http://shangetu{s}.map.bdimg.com/it/u=x={x};y={y};z={z};v=009;type=sate&fm=46
     - {s}: 0, 1, 2, 3
     Google Map: http://{s}.google.com/vt/lyrs={t}&x={x}&y={y}&z={z}
     - {s}: mt0, mt1, mt2, mt3
     - {t}: h = roads only, m = standard roadmap, p = colored terrain, s = satellite only, t = terrain only, y = hybrid
     Carto Voyager: https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png
     - {s}: a, b, c; {r}: no set or @2x
     Custom TMS from GlobalMapper:
     - options->setPluginData("UrlPathFunction", (void*)createGlobalMapperPath);
     - Custom function:
       static std::string createGlobalMapperPath(int type, const std::string& prefix, int x, int y, int z) {
           int newY = pow(2, z) + y, newZ = z + 1;  // if "OriginBottomLeft=1"
           //int newY = pow(2, z + 1) - y, newZ = z + 1;  // if "OriginBottomLeft=0"
           return osgVerse::TileCallback::createPath(prefix, x, newY, newZ); }
*/
class ReaderWriterTMS : public osgDB::ReaderWriter
{
public:
    ReaderWriterTMS()
    {
        supportsExtension("verse_tms", "osgVerse pseudo-loader");
        supportsExtension("tms", "TMS tile indices");
        supportsOption("URL", "The TMS server URL with wildcards, applied as orthophoto layer");
        supportsOption("Orthophoto", "TMS server URL with wildcards or .mbtiles, applied as orthophoto layer");
        supportsOption("Elevation", "TMS server URL with wildcards or .mbtiles, applied as elevation layer");
        supportsOption("OceanMask", "TMS server URL with wildcards or .mbtiles, applied as ocean mask layer");
        supportsOption("User", "TMS server URL with wildcards or .mbtiles, applied as extra overlay layer");
        supportsOption("Overlay", "TMS/WMTS server URL with wildcards, applied as second overlay layer (e.g. GIBS)");
        supportsOption("UrlPathFunction", "The custom function from setPluginData() to compute tile URL");
        supportsOption("UseEarth3D", "Display TMS tiles as a real earth: default=0");
        supportsOption("UseWebMercator", "Use Web Mercator (Level-0 has 4 tiles): default=0");
        supportsOption("OriginBottomLeft", "Use bottom-left as every tile's origin point: default=0");
        supportsOption("NoGlobeAttribute", "Donot use extra vertex-attribute for ocean-related data: default=0");
        supportsOption("FlatExtentMinX", "Flat earth extent X0: default -180");
        supportsOption("FlatExtentMinY", "Flat earth extent Y0: default -90");
        supportsOption("FlatExtentMaxX", "Flat earth extent X1: default 180");
        supportsOption("FlatExtentMaxY", "Flat earth extent Y1: default 90");
        supportsOption("MaximumLevel", "Set maximum level (Z) to load: default 0 (infinite)");
        supportsOption("TileSkirtRatio", "Create skirts for every tile: default 0.02");
        supportsOption("TileElevationScale", "Set elevation scale for every tile: default 1.0");
        osgDB::Registry::instance()->getReaderWriterForExtension("verse_web");
        osgDB::Registry::instance()->getReaderWriterForExtension("verse_webp");
    }

    virtual const char* className() const
    {
        return "[osgVerse] TMS tile reader";
    }

    virtual ReadResult readNode(const std::string& path, const Options* options) const
    {
        std::string fileName(path);
        std::string ext = osgDB::getLowerCaseFileExtension(path);
        if (!acceptsExtension(ext)) return ReadResult::FILE_NOT_HANDLED;

        bool usePseudo = (ext == "verse_tms"), useWM = true;
        if (usePseudo)
        {
            fileName = osgDB::getNameLessExtension(path);
            ext = osgDB::getLowerCaseFileExtension(fileName);
        }

        std::vector<std::string> values; osgDB::split(fileName, values, '-');
        if (options && values.size() > 2)
        {
            int x = atoi(values[0].c_str()) * 2, y = atoi(values[1].c_str()) * 2,
                z = (values[2] == "x") ? 0 : atoi(values[2].c_str()) + 1, countY = 2;

            std::string maxLvStr = options->getPluginStringData("MaximumLevel");
            int maxZ = atoi(maxLvStr.c_str());  // return OK but not loading anything
            if (maxZ > 0 && maxZ < z) return ReadResult::FILE_LOADED;

            std::string maskAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("OceanMask"));
            std::string elevAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("Elevation"));
            std::string orthoAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("Orthophoto"));
            if (orthoAddr.empty()) orthoAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("URL"));
            std::string userAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("User"));
            std::string overlayAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("Overlay"));

            osg::Vec3d extentMin = osg::Vec3d(-180.0, -90.0, 0.0), extentMax = osg::Vec3d(180.0, 90.0, 0.0);
            std::string strX = options->getPluginStringData("FlatExtentMinX"),
                        strY = options->getPluginStringData("FlatExtentMinY");
            if (!strX.empty()) extentMin[0] = atof(strX.c_str()); if (!strY.empty()) extentMin[1] = atof(strY.c_str());
            strX = options->getPluginStringData("FlatExtentMaxX"); strY = options->getPluginStringData("FlatExtentMaxY");
            if (!strX.empty()) extentMax[0] = atof(strX.c_str()); if (!strY.empty()) extentMax[1] = atof(strY.c_str());

            std::string use4T = options->getPluginStringData("UseWebMercator");
            if (!use4T.empty()) std::transform(use4T.begin(), use4T.end(), use4T.begin(), tolower);
            if (use4T == "false" || atoi(use4T.c_str()) <= 0)
            {
                extentMax[0] = (extentMax[0] + extentMin[0]) * 0.5;
                if (z == 0) countY = 1; useWM = false;
            }

            std::string useEarth = options->getPluginStringData("UseEarth3D");
            if (!useEarth.empty()) std::transform(useEarth.begin(), useEarth.end(), useEarth.begin(), tolower);
            bool flatten = (useEarth == "false" || atoi(useEarth.c_str()) <= 0);

            osg::ref_ptr<osg::Group> group = new osg::Group;
            group->setName("TMSGroup:" + fileName);
            for (int yy = 0; yy < countY; ++yy)
                for (int xx = 0; xx < 2; ++xx)
                {
                    osg::ref_ptr<osg::Node> node = createTile(
                        elevAddr, orthoAddr, maskAddr, userAddr, overlayAddr, x + xx, y + yy, z,
                        extentMin, extentMax, options, useWM, flatten);
                    if (!node) continue;

                    osg::ref_ptr<osg::PagedLOD> plod = new osg::PagedLOD;
                    plod->setDatabaseOptions(options->cloneOptions());
                    plod->addChild(node.get()); plod->setName("TMSLod:" + fileName);
                    plod->setFileName(1, std::to_string(x + xx) + "-" + std::to_string(y + yy) +
                                         "-" + std::to_string(z) + ".verse_tms");
                    plod->setRangeMode(osg::LOD::PIXEL_SIZE_ON_SCREEN);
                    if (flatten)
                        { plod->setRange(0, 0.0f, 500.0f); plod->setRange(1, 500.0f, FLT_MAX); }
                    else
                        { plod->setRange(0, 0.0f, 1000.0f); plod->setRange(1, 1000.0f, FLT_MAX); }
                    group->addChild(plod.get());
                }
            return (group->getNumChildren() > 0) ? group.get() : NULL;
        }
        return ReadResult::FILE_NOT_FOUND;
    }

protected:
    osg::Node* createTile(const std::string& elevPath, const std::string& orthPath,
                          const std::string& maskPath, const std::string& userPath,
                          const std::string& overlayPath, int x, int y, int z,
                          const osg::Vec3d& extentMin, const osg::Vec3d& extentMax,
                          const Options* opt, bool useWM, bool flatten) const
    {
        CreatePathFunc pathFunc = (CreatePathFunc)opt->getPluginData("UrlPathFunction");
        std::string name = "TMS_" + std::to_string(x) + "_" + std::to_string(y) + "_" + std::to_string(z),
                    botLeft = opt->getPluginStringData("OriginBottomLeft"),
                    noGlobeAttr = opt->getPluginStringData("NoGlobeAttribute");

        std::string elevPath1 = elevPath, ext = osgDB::getFileExtensionIncludingDot(elevPath), extWithVerse;
        size_t queryInExt = ext.find("?");  // remove query string if mixed with extension
        if (queryInExt != std::string::npos) ext = ext.substr(0, queryInExt);

        osgVerse::TileManager* mgr = osgVerse::TileManager::instance();
        bool elevH = mgr->isHandlerExtension(ext, extWithVerse), changed = false;
        if (!extWithVerse.empty() && osgDB::getServerProtocol(elevPath1).empty())  // auto-change local file ext
            osgVerse::TileCallback::replace(elevPath1, ext, extWithVerse, changed);

        osg::ref_ptr<osgVerse::TileCallback> tileCB = new osgVerse::TileCallback(atoi(noGlobeAttr.c_str()) == 0);
        tileCB->setLayerPath(osgVerse::TileCallback::ELEVATION, elevPath1);
        tileCB->setLayerPath(osgVerse::TileCallback::ORTHOPHOTO, orthPath);
        tileCB->setLayerPath(osgVerse::TileCallback::OCEAN_MASK, maskPath);
        tileCB->setLayerPath(osgVerse::TileCallback::USER, userPath);
        tileCB->setLayerPath(osgVerse::TileCallback::OVERLAY, overlayPath);
        tileCB->setTotalExtent(extentMin, extentMax); tileCB->setTileNumber(x, y, z);
        tileCB->setBottomLeft(atoi(botLeft.c_str()) > 0);
        tileCB->setUseWebMercator(useWM); tileCB->setFlatten(flatten);
        tileCB->setCreatePathFunction(pathFunc); tileCB->setName(name + "_Callback");
        if (opt)
        {
            std::string skirt = opt->getPluginStringData("TileSkirtRatio");
            if (!skirt.empty()) tileCB->setSkirtRatio((float)atof(skirt.c_str()));

            std::string scale = opt->getPluginStringData("TileElevationScale");
            if (!scale.empty()) tileCB->setElevationScale((float)atof(scale.c_str()));

            std::string elevEnc = opt->getPluginStringData("ElevationEncoding");
            std::transform(elevEnc.begin(), elevEnc.end(), elevEnc.begin(), ::tolower);
            if (elevEnc == "terrarium")
                tileCB->setElevationEncoding(osgVerse::TileCallback::TERRARIUM_ELEVATION);
            else
                tileCB->setElevationEncoding(osgVerse::TileCallback::RAW_ELEVATION);
        }

        osg::Vec3d tileMin, tileMax; double tileWidth = 0.0, tileHeight = 0.0;
        tileCB->computeTileExtent(tileMin, tileMax, tileWidth, tileHeight);

        osg::Matrix localMatrix; osg::ref_ptr<osg::Texture> elevImage;
        osg::ref_ptr<osgVerse::TileGeometryHandler> elevHandler;
        osg::ref_ptr<osg::Texture> orthImage, maskImage, userImage, overlayImage;
        // Each layer gets its own emptyPath flag (the layers now load concurrently, so the
        // old shared emptyPath0 between elevation and orthophoto would be a data race).
        bool emptyPathE = false, emptyPath0 = false, emptyPath1 = false, emptyPathU = false, emptyPathO = false;
        bool allLayersDone = true;
        osgVerse::TileCallback::LayerState failState = osgVerse::TileCallback::DEFERRED;

        // Load all layers concurrently (or inline when EARTH_TILE_POOL=0). The tasks share no
        // mutable state — each writes a distinct texture and a distinct emptyPath flag, and
        // createLayerImage/Handler only read tileCB. runAll() blocks until all finish, so the
        // captured stack locals stay alive throughout.
        std::vector<std::function<void()> > layerTasks;
        if (elevH)
            layerTasks.push_back([&]() { elevHandler = tileCB->createLayerHandler(osgVerse::TileCallback::ELEVATION, emptyPathE, opt); });
        else
            layerTasks.push_back([&]() { elevImage = tileCB->createLayerImage(osgVerse::TileCallback::ELEVATION, emptyPathE, opt); });
        layerTasks.push_back([&]() { orthImage = tileCB->createLayerImage(osgVerse::TileCallback::ORTHOPHOTO, emptyPath0, opt); });
        layerTasks.push_back([&]() { maskImage = tileCB->createLayerImage(osgVerse::TileCallback::OCEAN_MASK, emptyPath1, opt); });
        layerTasks.push_back([&]() { userImage = tileCB->createLayerImage(osgVerse::TileCallback::USER, emptyPathU, opt); });
        layerTasks.push_back([&]() { overlayImage = tileCB->createLayerImage(osgVerse::TileCallback::OVERLAY, emptyPathO, opt); });
        LayerLoadPool::instance().runAll(layerTasks);

        // Elevation is special: a 3D tile MUST have height data. If this tile has none of
        // its own — z>15 has no AWS terrarium data (createCustomPath returns ""), or a
        // transient fetch failure — defer it so operator()/updateLayerData inherits the
        // parent tile's elevation. Otherwise the tile is built flat at sea level, which on
        // high terrain (e.g. Kunming ~1890 m) sinks deep tiles below their z15 parent and
        // shows them parallax-misaligned. elevPath empty = elevation not configured at all
        // → leave flat (nothing to inherit).
        if (!elevHandler && !elevImage && !elevPath.empty())
            { tileCB->setLayerPathState(osgVerse::TileCallback::ELEVATION, failState); allLayersDone = false; }
        if (!orthImage && !emptyPath0)
            { tileCB->setLayerPathState(osgVerse::TileCallback::ORTHOPHOTO, failState); allLayersDone = false; }
        // Like elevation, the land/ocean mask must inherit from the parent when this tile
        // has none of its own (Mask_lv3 is z<=3 only, so createCustomPath returns "" past
        // z3). Otherwise the mask reads 0 = "ocean" everywhere, and with the ocean pass on,
        // z>3 tiles get the ocean tint over land too — dark blue rectangles at the z3/z4 LOD
        // boundary. The mask also drives terrain relief shading, so inheriting keeps that
        // continuous across the boundary. maskPath empty = no mask configured → skip.
        if (!maskImage && !maskPath.empty())
            { tileCB->setLayerPathState(osgVerse::TileCallback::OCEAN_MASK, failState); allLayersDone = false; }
        if (!userImage && !emptyPathU)
            { tileCB->setLayerPathState(osgVerse::TileCallback::USER, failState); allLayersDone = false; }
        if (!overlayImage && !emptyPathO)
            { tileCB->setLayerPathState(osgVerse::TileCallback::OVERLAY, failState); allLayersDone = false; }
        tileCB->setLayersDone(allLayersDone);

        // FIXME: this makes no future tiles... consider a better way?
        if (!orthImage) { OSG_NOTICE << "[ReaderWriterTMS] No imagery for tile " << name << "\n"; return NULL; }

        osg::ref_ptr<osg::Geometry> geom = elevHandler.valid() ?
            tileCB->createTileGeometry(localMatrix, elevHandler.get(), tileMin, tileMax, tileWidth, tileHeight) :
            tileCB->createTileGeometry(localMatrix, elevImage.get(), tileMin, tileMax, tileWidth, tileHeight);
        geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true); geom->setName(name + "_Geom");
        if (orthImage.valid())
        {
            orthImage->setClientStorageHint(false);
            orthImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(0, orthImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(0, orthImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset1", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }

        if (maskImage.valid())
        {
            maskImage->setClientStorageHint(false);
            maskImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(1, maskImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(1, maskImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset2", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }

        if (userImage.valid())
        {
            userImage->setClientStorageHint(false);
            userImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(2, userImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(2, userImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset3", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }

        if (overlayImage.valid())
        {
            overlayImage->setClientStorageHint(false);
            overlayImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(3, overlayImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(3, overlayImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset4", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom.get()); geode->setName(name + "_Geode");

        osg::ref_ptr<osg::MatrixTransform> mt = new osg::MatrixTransform;
        mt->setUpdateCallback(tileCB.get());
        mt->setName(name); mt->addChild(geode.get());
        mt->setMatrix(localMatrix); return mt.release();
    }
};

// Now register with Registry to instantiate the above reader/writer.
REGISTER_OSGPLUGIN(verse_tms, ReaderWriterTMS)
