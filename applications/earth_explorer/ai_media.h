#ifndef EARTH_AI_MEDIA_H
#define EARTH_AI_MEDIA_H
// 生成式媒体管线(Task 8 生图 + Task 9 首尾帧巡航视频):抓当前渲染帧 → 喂给 Gemini
// 图像模型生成实拍照片,或抓两帧(A/B 两点位姿)喂给 Veo 生成运镜视频。结果推到右上角
// 卡片(AICardPanel)。Job 驱动,主线程每帧 update() 轮询;耗时的 HTTP 调用放独立 worker
// 线程,绝不阻塞渲染主线程(与 AIChatCore 的 worker 模式一致)。
#include "ai_tools.h"
#include "ai_motion.h"
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

    // Veo 首尾帧视频 Provider:输入 A/B 两帧 PNG 字节 + 运动提示词,提交长任务拿到
    // operation 名字;轮询该 operation 直到完成,取回 mp4 字节。两步都同步阻塞,
    // 供独立的轮询 worker 线程调用(不得在主线程调,轮询要跨越数十秒到数分钟)。
    class VeoVideoProvider
    {
    public:
        VeoVideoProvider(const std::string& apiKey, const std::string& model);

        // 提交生成请求(predictLongRunning),成功返回 operation 名字(如
        // "models/veo-3.1-generate-001/operations/xxxx"),失败返回空串并填 err。
        std::string submit(const std::string& pngBytesA, const std::string& pngBytesB,
                           const std::string& motionPrompt, std::string& err);

        // 轮询一次 operation 状态。done=true 且 mp4Bytes 非空 → 已拿到视频字节;
        // done=true 但 mp4Bytes 为空且 err 非空 → 服务端报错,视为终态失败;
        // done=false → 仍在跑,调用方稍后重试。uri 形式的结果本函数内部会再发一次
        // GET 把字节拉下来(uri 可能需要拼 ?key=,见 .cpp)。
        void poll(const std::string& operationName, bool& done, std::string& mp4Bytes, std::string& err);

    private:
        std::string _apiKey, _model;
    };

    // Omni(Interactions API,/v1beta/interactions)单图+提示词**同步**生视频。
    // 模型名含 "omni" 时 MediaManager::confirmVideo 走此路径而非 Veo predictLongRunning。
    // 官方明确:Omni 不支持首尾帧插值(video interpolation)——B 点画面不上传,
    // 只通过运动提示词参与;要严格首尾帧穿越请 EARTH_AI_VIDEO_MODEL 切回 veo-3.1-*。
    class OmniVideoProvider
    {
    public:
        OmniVideoProvider(const std::string& apiKey, const std::string& model);
        // 同步调用(worker 线程,可能耗时 30-180s):成功返回 true 并填 mp4Bytes;
        // 失败返回 false 并填 err。响应里视频可能是 steps[].content[] 的 base64,
        // 也可能是 output_video.uri(需再 GET 下载),两种形状都处理。
        bool generate(const std::string& firstPngBytes, const std::string& motionPrompt,
                      std::string& mp4Bytes, std::string& err);
    private:
        std::string _apiKey, _model;
    };

    // videoPhase() 的返回类型,给 UI 判断三态按钮/是否弹 Modal 用。放 MediaManager 类外
    // 是因为 ai_ui.cpp 需要在头文件之外看到这个类型名,且 MediaManager 类体内(下方)
    // 就要用到它作为 videoPhase() 的返回类型,必须先声明。
    enum VideoPhaseKindPublic
    {
        VIDEO_IDLE = 0,          // 空闲:按钮显示"视频"
        VIDEO_WAIT_A,            // 已点按钮,A 点快照抓取中(还没稳定)
        VIDEO_WAIT_B,            // A 点已就绪,等待用户移动相机后再次触发 B 点采集
        VIDEO_CAPTURING_B,       // 已触发 B 点采集,B 点快照抓取中
        VIDEO_AWAIT_CONFIRM,     // A/B 都就绪,等待用户在确认 Modal 里点「确认」
        VIDEO_RUNNING            // 已确认,Job 在跑(提交/轮询/下载)
    };

    // 生成式媒体管线总控:Job 驱动,每帧 update() 由 AIFrameHandler 调用(主线程)。
    // 照片与视频各自只支持"单个 pending 任务"——与真实使用场景(用户点一次等一次)相符,
    // 并发第二个请求会被 startPhotoJob/beginVideoCapture 拒绝,避免状态机复杂化。
    class MediaManager
    {
    public:
        // apiKeyOrEmpty 为空 → 仅在 EARTH_AI_FAKE_IMG/EARTH_AI_FAKE_MP4 设置时可用(离线 E2E);
        // 两者都没有的话 startPhotoJob/confirmVideo 会返回 error(由调用方/工具层处理)。
        MediaManager(osgViewer::Viewer* viewer, AICardPanel* cards, const std::string& apiKeyOrEmpty);
        ~MediaManager();

        JobManager* jobs() { return &_jobs; }

        // generate_photo 工具入口(主线程调用,在 AIChatCore::drainMainThread 的工具执行阶段):
        // 建 Job → 触发抓帧 → 立即返回 {"status":"started","job_id":N}(不等生图完成)。
        // 若已有照片任务在跑,返回 {"error":"photo job already running"}。
        picojson::value startPhotoJob(const std::string& stylePrompt, double lat, double lon,
                                      double altKm, bool haveView);

        void update();   // 主线程每帧调:轮询抓帧就绪 → 起工作线程生图/生视频 → 完成后推卡片/收尾 Job

        // ---- 视频两点巡航流程(Task 9):A 点 -> B 点 -> 确认 Modal -> 提交 -> 轮询 -> 完成 ----
        // 状态机见 .cpp VideoPhase 注释。UI(🎬 按钮/确认 Modal)与 generate_video 工具
        // 共用同一套状态,任何入口都必须先过两点采集 + 确认这两步,不允许绕过。

        // 当前视频流程所处阶段,UI(三态按钮/确认 Modal)与 generate_video 工具都据此判断
        // 该进入哪一步——VIDEO_IDLE 即"没有视频任务在跑",调用方按需自行与该值比较
        // (未单独提供 videoBusy() 之类的派生便利函数,phase != VIDEO_IDLE 已经足够直白)。
        VideoPhaseKindPublic videoPhase() const;

        // 记录 A 点(抓快照 + 记相机位姿)。若已有视频任务在跑(非 IDLE)返回 false。
        bool beginVideoCapture(const osg::Vec3d& llaA);
        // 记录 B 点。要求当前处于"已录 A、等待 B"阶段,否则返回 false。
        bool captureVideoEnd(const osg::Vec3d& llaB);
        // 两点都已抓到快照文件后才能取(A/B 快照仍在写盘时返回 false)。
        // 供 UI 确认 Modal 展示:A/B 坐标 + 运动提示词预览 + 状态是否就绪。
        struct PendingVideoInfo
        {
            bool ready = false;      // 两点快照都已就绪,可以画确认 Modal 了
            osg::Vec3d llaA, llaB;
            std::string motionPrompt;
        };
        PendingVideoInfo pendingVideoInfo() const;

        // 用户在确认 Modal 里点「确认生成」:真正建 Job、起 worker 提交 Veo 请求。
        // 要求当前处于 AWAIT_CONFIRM 阶段,否则返回 error(不消费任何状态)。
        picojson::value confirmVideo();
        // 用户点「取消」或按 ESC:整个视频流程清零回 IDLE,不产生任何费用。
        void cancelVideo();

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

        // 每帧驱动视频状态机(照片流程用内联在 update() 里的逻辑,视频流程状态更多,
        // 拆一个私有方法、被 update() 末尾调用,避免 update() 单个函数过长)。
        void updateVideoInternal();

        // 安全地把 *_video 重置为初始状态:先 join 掉可能还 joinable 的 worker 线程,
        // 再做 *_video = VideoJob()(move-assign)。std::thread 的 move 赋值要求目标线程
        // 对象此刻不 joinable,否则直接 std::terminate——本函数是 updateVideoInternal()/
        // cancelVideo() 里所有"重置视频状态"的唯一入口,不再各处手写裸的
        // "*_video = VideoJob()",避免遗漏 join 埋雷。
        void resetVideo();

        // ---- 视频状态机内部实现(.cpp 里定义完整 enum VideoPhase,这里只前置声明用到的类型)----
        struct VideoJob;   // .cpp 内定义:review 建议的 PendingJob 提取,视频专用(字段与照片不同,
                           // 独立结构体比硬凑一个通用 PendingJob 更清楚——见 .cpp 头注释)。
        VideoJob* _video;  // 指针以避免本头文件暴露 VideoJob 定义(pimpl 风格,video 专属状态)

        // 独立的第二个 SnapshotGrabber:照片流程的 _grabber 与视频的 A/B 快照都可能同时
        // "在等待稳定"(用户点了视频 A 点又几乎同时点了照片按钮),共用一个 SnapshotGrabber
        // 会导致 grab() 互相覆盖对方的捕获目标。两条流程完全独立、开销可忽略(只是一个
        // ScreenCaptureHandler),分开更安全。
        SnapshotGrabber _videoGrabber;
    };
}
#endif
