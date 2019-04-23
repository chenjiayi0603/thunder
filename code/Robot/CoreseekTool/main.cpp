/*******************************************************************************
* Project:  CoreseekTool
* @file     main.cpp
* @brief 
* @author   chenjiayi
* @date:    2015年11月23日
* @note
* Modify history:
******************************************************************************/
#include "CoreSeekMgr.h"

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2)
    {
        std::cerr << "para num error!" << std::endl;
        exit(-1);
    }
    ngx_init_setproctitle(argc, argv);
    robot::CoreSeekMgr oManager(argv[1]);
    oManager.Run();//执行完后自动退出
    return(0);
}
