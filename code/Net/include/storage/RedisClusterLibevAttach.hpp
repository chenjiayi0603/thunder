#pragma once

#include "hiredis_vip/adapters/libev.h"
#include "hiredis_vip/hircluster.h"

#ifdef __cplusplus
extern "C" {
#endif

/** hiredis-vip 的 adapters/libev.h 仅有单节点 redisLibevAttach；集群逻辑与 adapters/libevent.h
 *  中 redisClusterLibeventAttach 相同：由 hircluster 在创建各节点 redisAsyncContext 时调用 attach_fn。 */
static int redisLibevAttach_link(redisAsyncContext *ac, void *loop_void)
{
    struct ev_loop *loop = (struct ev_loop *)loop_void;
#if EV_MULTIPLICITY
    if (loop == NULL)
    {
        return REDIS_ERR;
    }
    return redisLibevAttach(loop, ac);
#else
    (void)loop;
    return redisLibevAttach(ac);
#endif
}

static int redisClusterLibevAttach(redisClusterAsyncContext *acc, struct ev_loop *loop)
{
    if (acc == NULL || loop == NULL)
    {
        return REDIS_ERR;
    }
    acc->adapter = loop;
    acc->attach_fn = redisLibevAttach_link;
    return REDIS_OK;
}

#ifdef __cplusplus
}
#endif
