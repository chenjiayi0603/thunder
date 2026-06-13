#ifndef MODULEHELLO_HELLOCO_REDIS_HPP_
#define MODULEHELLO_HELLOCO_REDIS_HPP_

#include <string>
#include "coro/Coroutine20.hpp"

namespace net { class StepCo20; }

net::AsyncTask HelloCoRedisCo(net::StepCo20& step, std::string redisHost, int redisPort);

#endif
