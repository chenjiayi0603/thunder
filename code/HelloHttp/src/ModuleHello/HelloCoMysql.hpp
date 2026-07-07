#ifndef MODULEHELLO_HELLOCO_MYSQL_HPP_
#define MODULEHELLO_HELLOCO_MYSQL_HPP_

#include "coro/Coroutine20.hpp"
#include "dbi/Dbi.hpp"

namespace net { class StepCo20; }

net::AsyncTask HelloCoMysqlCo(net::StepCo20& step, util::tagDbConnInfo dbConn);

#endif
