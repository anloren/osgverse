#ifndef EARTH_AI_TOOLS_H
#define EARTH_AI_TOOLS_H
// AI Chat 核心抽象:一切能力皆 Tool。本头文件不依赖 OSG,可被 tests/ 单测。
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <picojson.h>

namespace earthai
{
    struct Tool
    {
        std::string name, description;
        std::string parametersJson;   // JSON schema(object)字符串,直接嵌入 declarations
        // 主线程执行(由 AIChatCore 的 FRAME drain 保证);返回值回传给模型
        std::function<picojson::value(const picojson::value& args)> execute;
    };

    class ToolRegistry
    {
    public:
        void add(const Tool& t) { _tools.push_back(t); }
        const std::vector<Tool>& tools() const { return _tools; }

        // Gemini functionDeclarations 数组 JSON
        std::string buildDeclarationsJson() const
        {
            std::string s = "[";
            for (size_t i = 0; i < _tools.size(); ++i)
            {
                const Tool& t = _tools[i];
                picojson::object o;
                o["name"] = picojson::value(t.name);
                o["description"] = picojson::value(t.description);
                s += picojson::value(o).serialize();
                // parameters 是现成 JSON 字符串,拼接进对象(避免二次解析)
                s.insert(s.size() - 1, ",\"parameters\":" + t.parametersJson);
                if (i + 1 < _tools.size()) s += ",";
            }
            return s + "]";
        }

        bool dispatch(const std::string& name, const picojson::value& args, picojson::value& result)
        {
            for (size_t i = 0; i < _tools.size(); ++i)
                if (_tools[i].name == name) { result = _tools[i].execute(args); return true; }
            picojson::object err; err["error"] = picojson::value("unknown tool: " + name);
            result = picojson::value(err); return false;
        }
    private:
        std::vector<Tool> _tools;
    };

    struct AIJob
    {
        enum Status { PENDING = 0, RUNNING, DONE, FAILED };
        int id = 0; std::string kind, title;
        Status status = PENDING; float progress = 0.0f;
        std::string resultPath, error;
    };

    class JobManager   // 全部方法线程安全(生成任务在后台线程更新,UI 主线程读)
    {
    public:
        int create(const std::string& kind, const std::string& title)
        {
            std::lock_guard<std::mutex> g(_mutex);
            AIJob j; j.id = ++_nextId; j.kind = kind; j.title = title;
            _jobs[j.id] = j; return j.id;
        }
        void update(int id, AIJob::Status st, float progress,
                    const std::string& resultPath, const std::string& error)
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::map<int, AIJob>::iterator it = _jobs.find(id); if (it == _jobs.end()) return;
            it->second.status = st; it->second.progress = progress;
            // 哨兵语义:空字符串代表"本次调用不更新该字段",保留旧值;
            // 因此调用方若想真正清空 resultPath/error,不能传空串(当前接口不支持,按需再加显式清空方法)
            if (!resultPath.empty()) it->second.resultPath = resultPath;
            if (!error.empty()) it->second.error = error;
        }
        bool get(int id, AIJob& out) const
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::map<int, AIJob>::const_iterator it = _jobs.find(id);
            if (it == _jobs.end()) return false; out = it->second; return true;
        }
        // 仅推进"进行中"的进度条,不触碰 status/resultPath/error——供主线程每帧"进度爬升"
        // 用,与后台 worker 写 DONE/FAILED 的 update() 调用天然互斥于同一把锁下:worker 若已经
        // 把 status 改成 DONE/FAILED,这里读到的 status 就不是 RUNNING,直接跳过、不会把已经
        // 完成的任务重新"看起来还在跑"(get 一次+update 一次这种 read-modify-write 不在锁内
        // 完成的话,中间可能被 worker 的 update() 插入,导致 status 被这次 creep 覆盖回
        // RUNNING——本方法把判断和写入放进同一次加锁,消除这个竞态)。
        // 返回值:true=确实推进了(仍是 RUNNING);false=job 不存在或已不是 RUNNING(未做任何修改)。
        bool creepProgress(int id, float progress)
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::map<int, AIJob>::iterator it = _jobs.find(id);
            if (it == _jobs.end() || it->second.status != AIJob::RUNNING) return false;
            it->second.progress = progress;
            return true;
        }
        std::vector<AIJob> list() const
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::vector<AIJob> v; for (std::map<int, AIJob>::const_iterator it = _jobs.begin();
                                        it != _jobs.end(); ++it) v.push_back(it->second);
            return v;
        }
    private:
        mutable std::mutex _mutex; std::map<int, AIJob> _jobs; int _nextId = 0;
    };
}
#endif
