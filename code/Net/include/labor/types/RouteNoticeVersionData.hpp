/*******************************************************************************
* Project:  Thunder
* @file     RouteNoticeVersionData.hpp
* @brief    Shared route/node-notice data between processes
******************************************************************************/
#ifndef SRC_LABOR_TYPES_ROUTE_NOTICE_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_ROUTE_NOTICE_VERSION_DATA_HPP_

#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"
#include "protocol/oss_sys.pb.h"

struct RouteNoticeVersionData
{
    struct RouteNoticeVersionMM
    {
        uint64 seq_node_notice = 0;
        uint32 node_id = 0;
        uint32 node_notice_len = 0;
        // route mirror blob size (bytes). Keep writer/readers in sync.
        char node_notice_blob[160 * 1024] = {0};
    };

private:
    RouteNoticeVersionMM* m_pShm = nullptr;
    struct
    {
        uint64 node_notice = 0;
    } m_ack{};

public:
    void SetRouteNoticeVersionMM(RouteNoticeVersionMM* routeNoticeVersionMM = nullptr)
    {
        if (routeNoticeVersionMM)
        {
            DelRouteNoticeVersionMM();
            m_pShm = routeNoticeVersionMM;
        }
        else if (m_pShm == nullptr)
        {
            m_pShm = static_cast<RouteNoticeVersionMM*>(mmap(
                NULL, sizeof(RouteNoticeVersionMM), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
            memset(m_pShm, 0, sizeof(*m_pShm));
        }
    }

    RouteNoticeVersionMM* GetRouteNoticeVersionMM()
    {
        if (m_pShm == nullptr)
        {
            SetRouteNoticeVersionMM();
        }
        return m_pShm;
    }

    void DelRouteNoticeVersionMM()
    {
        if (m_pShm)
        {
            munmap(m_pShm, sizeof(*m_pShm));
            m_pShm = nullptr;
        }
    }

    bool SetNodeNotice(const NodeNotice& oNodeNotice)
    {
        const int bs = oNodeNotice.ByteSize();
        if (!m_pShm || bs <= 0 || bs >= static_cast<int>(sizeof(m_pShm->node_notice_blob)))
        {
            return false;
        }
        const std::string data = oNodeNotice.SerializeAsString();
        memcpy(m_pShm->node_notice_blob, data.data(), data.size());
        m_pShm->node_notice_len = static_cast<uint32_t>(data.size());
        ++m_pShm->seq_node_notice;
        return true;
    }

    bool GetNodeNotice(NodeNotice& oNodeNotice) const
    {
        if (!m_pShm || m_pShm->node_notice_len == 0)
        {
            return false;
        }
        return oNodeNotice.ParseFromArray(m_pShm->node_notice_blob, static_cast<int>(m_pShm->node_notice_len));
    }

    void UpdateNodeNoticeVersion()
    {
        if (m_pShm)
        {
            m_ack.node_notice = m_pShm->seq_node_notice;
        }
    }

    bool IsNodeNoticeVersionChange() const
    {
        return m_pShm && (m_pShm->seq_node_notice > m_ack.node_notice);
    }

    uint64 GetNodeNoticeVersion() const { return m_pShm ? m_pShm->seq_node_notice : 0; }
    uint32 GetNodeNoticeLength() const { return m_pShm ? m_pShm->node_notice_len : 0; }

    void SetNodeId(uint32 iNodeId)
    {
        if (m_pShm)
        {
            m_pShm->node_id = iNodeId;
        }
    }

    uint32 GetNodeId() const { return m_pShm ? m_pShm->node_id : 0; }
};

#endif /* SRC_LABOR_TYPES_ROUTE_NOTICE_VERSION_DATA_HPP_ */
