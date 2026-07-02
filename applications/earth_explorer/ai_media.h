#ifndef EARTH_AI_MEDIA_H
#define EARTH_AI_MEDIA_H
// 生成式媒体管线(Task 8):抓当前渲染帧 → 喂给 Gemini 图像模型 → 生成实拍照片,
// 结果推到右上角卡片(AICardPanel)。Job 驱动,主线程每帧 update() 轮询;耗时的
// HTTP 调用放独立 worker 线程,绝不阻塞渲染主线程(与 AIChatCore 的 worker 模式一致)。
#include "ai_tools.h"
#include <osg/Vec3d>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <picojson.h>
#include <ios>
#include <string>
#include <thread>

class AICardPanel;

namespace earthai
{
    // 抓当前帧到 PNG 文件。基于 osgViewer::ScreenCaptureHandler(EARTH_AUTOCAP 同款),
    // 挂到 viewer 上按需触发单帧捕获;写盘由捕获回调在渲染后完成(异步:调用 grab() 之后
    // 要过几帧文件才会出现,ready() 供轮询)。
    // handler 只在构造时创建并 addEventHandler 一次,grab() 复用同一个 handler(只换写盘目标),
    // 避免每次 grab() 都新增一个 handler 导致 viewer 上 handler 无限累积(见 .cpp 注释)。
    class SnapshotGrabber
    {
    public:
        explicit SnapshotGrabber(osgViewer::Viewer* viewer);
        void grab(const std::string& pngPath);   // 触发一次抓帧(覆盖写);同时重置跨帧稳定性状态

        // 真正的跨帧稳定性判断:调用方(MediaManager::update())每帧调一次 ready()。
        // 本次看到的文件大小与"上一次调用 ready() 时"记录的大小相比——只有连续两次不同的
        // update() tick 都测到同一个 >0 大小,才认为写盘已完成。单次调用内部不再做"读两次
        // 比较"那种伪稳定性判断(两次 stat 间隔太短,几乎总能读到同一个值,写到一半也会
        // 误判为稳定)。_lastSize 在 grab() 时清零,避免复用上一次抓帧遗留的大小造成
        // 首次 ready() 就误判稳定。
        bool ready(const std::string& pngPath);

    private:
        osgViewer::Viewer* _viewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> _capturer;  // 唯一实例,构造时创建并挂一次
        std::streamsize _lastSize;   // 上一次 ready() 调用时测到的文件大小,0=尚未测到/已重置
    };

    // Gemini 图像生成 Provider:输入渲染帧 PNG 字节 + 提示词,同步阻塞调用(供 worker 线程用)。
    class GeminiMediaProvider
    {
    public:
        explicit GeminiMediaProvider(const std::string& apiKey);
        // 成功返回生成图 PNG 字节(非空);失败返回空字符串,err 写入原因。key 不进日志。
        std::string generateImage(const std::string& pngBytes, const std::string& prompt,
                                  std::string& err);

    private:
        std::string _apiKey;
    };

    // 生成式媒体管线总控:Job 驱动,每帧 update() 由 AIFrameHandler 调用(主线程)。
    // 当前只支持"单个 pending 照片任务"——与真实使用场景(用户点一次等一次)相符,
    // 并发第二个请求会被 startPhotoJob 拒绝(见 .cpp 注释),避免状态机复杂化。
    class MediaManager
    {
    public:
        // apiKeyOrEmpty 为空 → 仅在 EARTH_AI_FAKE_IMG 设置时可用(离线 E2E);
        // 两者都没有的话 startPhotoJob 会返回 error(由调用方/工具层处理)。
        MediaManager(osgViewer::Viewer* viewer, AICardPanel* cards, const std::string& apiKeyOrEmpty);
        ~MediaManager();

        JobManager* jobs() { return &_jobs; }

        // generate_photo 工具入口(主线程调用,在 AIChatCore::drainMainThread 的工具执行阶段):
        // 建 Job → 触发抓帧 → 立即返回 {"status":"started","job_id":N}(不等生图完成)。
        // 若已有照片任务在跑,返回 {"error":"photo job already running"}。
        picojson::value startPhotoJob(const std::string& stylePrompt, double lat, double lon,
                                      double altKm, bool haveView);

        void update();   // 主线程每帧调:轮询抓帧就绪 → 起工作线程生图 → 完成后推卡片/收尾 Job

    private:
        enum PendingState { IDLE, WAITING_SNAPSHOT, GENERATING, DONE_HANDLED };

        osgViewer::Viewer* _viewer;
        AICardPanel* _cards;
        std::string _apiKey;
        SnapshotGrabber _grabber;
        JobManager _jobs;

        // 单个 pending 照片任务的状态机(见类注释:同一时刻只支持一个)。
        PendingState _state;
        int _jobId;
        std::string _snapPath, _genPath, _prompt;
        std::thread _worker;
        bool _workerJoinable;
        int _waitSnapshotTicks;  // 进入 WAITING_SNAPSHOT 后累计的 update() 调用次数,超阈值判超时

        void joinWorkerIfAny();
    };
}
#endif
