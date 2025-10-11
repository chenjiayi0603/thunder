/*
 * Loader.hpp
 *
 *  Created on: 2021年2月24日
 *      Author: lm26
 */
#include <unordered_map>
#include "Worker.hpp"

namespace net
{

//配置加载进程
class Loader : public Worker
{
protected:
	virtual bool Init(util::CJsonObject& oJsonConf);
public:
	Loader(const std::string& strWorkPath,const std::string& strConfFile,util::CJsonObject& oJsonConf,LoaderConfigVersionData::LoaderConfigVersionMM *pLoaderConfigVersionMM,bool boRestart = false);
    ~Loader() = default;

    static void PeriodicTaskCallback(struct ev_loop* loop, struct ev_timer* watcher, int revents);  // 周期任务回调，用于替换IdleCallback

    void GetConfig(util::CJsonObject& oJsonConf,bool boRestart);

    void Run();

    bool CreateEvents();

    bool AddPeriodicTaskEvent();

    bool CheckParent();

    void SetProcessName(const util::CJsonObject& oJsonConf);

};

}
