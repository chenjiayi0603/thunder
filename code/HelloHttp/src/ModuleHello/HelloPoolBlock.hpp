#ifndef MODULEHELLO_HELLO_POOL_BLOCK_HPP_
#define MODULEHELLO_HELLO_POOL_BLOCK_HPP_

#include "coro/Coroutine20.hpp"

namespace net { class StepCo20; }

net::AsyncTask HelloPoolBlockCo(net::StepCo20& step);

#endif
