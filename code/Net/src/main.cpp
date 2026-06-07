/*******************************************************************************
* Project:  Net
* @file     main.cpp
* @brief    Thunder node entry — 支持 nginx 风格信号管理
*
* 用法:
*   Hello config.json             启动 (默认)
*   Hello -s reload  config.json  重载配置 (SIGUSR1)
*   Hello -s restart config.json  优雅重启所有 Worker (SIGUSR2)
*   Hello -s stop    config.json  优雅停止 (SIGTERM)
*
* 对照 nginx:
*   nginx -s reload  → kill -HUP  → Thunder: kill -SIGUSR1
*   nginx -s reopen  → kill -USR1 → Thunder: kill -SIGUSR1
*   nginx -s stop    → kill -TERM → Thunder: kill -SIGTERM
*   nginx -s quit    → kill -QUIT → Thunder: kill -SIGTERM
******************************************************************************/

#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include "unix/proctitle_helper.h"
#include "labor/Manager.hpp"

static std::string PidFile(const char* conf_path)
{
    std::string path(conf_path);
    size_t pos = path.rfind('/');
    std::string dir = (pos != std::string::npos) ? path.substr(0, pos + 1) : "./";
    return dir + "thunder.pid";
}

static bool WritePidFile(const char* conf_path, pid_t pid)
{
    std::ofstream f(PidFile(conf_path));
    if (!f) return false;
    f << pid;
    return true;
}

static pid_t ReadPidFile(const char* conf_path)
{
    std::ifstream f(PidFile(conf_path));
    if (!f) return -1;
    pid_t pid;
    f >> pid;
    return (f && pid > 0) ? pid : -1;
}

static bool SendSignal(const char* conf_path, int sig)
{
    pid_t pid = ReadPidFile(conf_path);
    if (pid <= 0)
    {
        std::cerr << "thunder: no pid file found (" << PidFile(conf_path)
                  << "), is it running?" << std::endl;
        return false;
    }
    if (kill(pid, 0) != 0)
    {
        std::cerr << "thunder: process " << pid << " not running" << std::endl;
        return false;
    }
    if (kill(pid, sig) != 0)
    {
        std::cerr << "thunder: kill(" << pid << ", " << sig << ") failed: "
                  << strerror(errno) << std::endl;
        return false;
    }
    std::cout << "thunder: signal " << sig << " sent to pid " << pid << std::endl;
    return true;
}

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);

    // ── nginx 风格: Hello -s <action> <config> ────────────
    if (argc >= 3 && strcmp(argv[1], "-s") == 0)
    {
        const char* action = argv[2];
        const char* conf   = (argc >= 4) ? argv[3] : "conf/Hello.json";
        int sig = 0;

        if (strcmp(action, "reload") == 0)
            sig = SIGUSR1;
        else if (strcmp(action, "restart") == 0)
            sig = SIGUSR2;
        else if (strcmp(action, "stop") == 0 || strcmp(action, "quit") == 0)
            sig = SIGTERM;
        else
        {
            std::cerr << "thunder: unknown action '" << action
                      << "'. Use: reload | restart | stop | quit" << std::endl;
            return 1;
        }
        return SendSignal(conf, sig) ? 0 : 1;
    }

    // ── 启动模式 ──────────────────────────────────────────
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <config.json>" << std::endl;
        std::cerr << "       " << argv[0] << " -s reload|restart|stop <config.json>" << std::endl;
        return 1;
    }

	ngx_init_setproctitle(argc, argv);

    net::Manager* pManager = new net::Manager(argv[1]);

    // 写 PID 文件 (在 Manager 初始化之后, 确保 PID 正确)
    // 先删旧的防止 hostPath 挂载残留上次运行的 PID
    std::remove(PidFile(argv[1]).c_str());
    WritePidFile(argv[1], getpid());

    pManager->Run();
    delete pManager;
	return 0;
}
