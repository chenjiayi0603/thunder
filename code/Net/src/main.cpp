/*******************************************************************************
* Project:  Net
* @file     main.cpp
* @brief 
* @author   cjy
* @date:    2019年8月6日
* @note
* Modify history:
******************************************************************************/

#include "unix/proctitle_helper.h"
#include "labor/Manager.hpp"

int main(int argc, char* argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2)
    {
        std::cerr << "para num error!" << std::endl;
        exit(-1);
    }
	ngx_init_setproctitle(argc, argv);
    net::Manager* pManager = new net::Manager(argv[1]);
    pManager->Run();
    delete pManager;
	return(0);
}
