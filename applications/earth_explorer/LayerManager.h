#ifndef EARTH_LAYER_MANAGER_H
#define EARTH_LAYER_MANAGER_H

#include <string>
#include <vector>
#include <functional>

// 统一图层注册表。P0 只管标注层；结构留好分组/类型/透明度，供后续 P1+ 扩展。
struct OverlayLayer
{
    enum Type { RasterTile, PointFeed, Grid };
    std::string id, displayName, group;
    Type type = RasterTile;
    bool enabled = false;
    bool hasOpacity = false;     // 栅格层有透明度滑块
    float opacity = 1.0f;
    bool needsKey = false;       // UI 标 key、缺 key 时灰显
    // 应用回调：enabled/opacity 变化时被调用（P0 里标注层把它接到 LabelOpacity uniform）
    std::function<void(const OverlayLayer&)> apply;
};

class LayerManager
{
public:
    OverlayLayer& add(const OverlayLayer& l) { _layers.push_back(l); return _layers.back(); }
    std::vector<OverlayLayer>& layers() { return _layers; }

    void setEnabled(const std::string& id, bool on)
    {
        if (OverlayLayer* l = find(id)) { l->enabled = on; if (l->apply) l->apply(*l); }
    }
    void setOpacity(const std::string& id, float v)
    {
        if (OverlayLayer* l = find(id)) { l->opacity = v; if (l->apply) l->apply(*l); }
    }
    OverlayLayer* find(const std::string& id)
    {
        for (size_t i = 0; i < _layers.size(); ++i)
            if (_layers[i].id == id) return &_layers[i];
        return nullptr;
    }
private:
    std::vector<OverlayLayer> _layers;
};

#endif
