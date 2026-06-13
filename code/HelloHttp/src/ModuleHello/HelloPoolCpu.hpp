#ifndef MODULEHELLO_HELLO_POOL_CPU_HPP_
#define MODULEHELLO_HELLO_POOL_CPU_HPP_

#include "coro/Coroutine20.hpp"

namespace net { class StepCo20; }

net::AsyncTask HelloPoolCpuCo(net::StepCo20& step);

#endif
