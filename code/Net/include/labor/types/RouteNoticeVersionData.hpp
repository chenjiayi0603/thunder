/*******************************************************************************
* Project:  Thunder
* @file     RouteNoticeVersionData.hpp
* @brief    Shared route/node-notice data between processes
******************************************************************************/
#ifndef SRC_LABOR_TYPES_ROUTE_NOTICE_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_ROUTE_NOTICE_VERSION_DATA_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"
#include "protocol/oss_sys.pb.h"

struct RouteNoticeVersionData
{
    struct RouteNoticeVersionMM
    {
        std::atomic<uint64_t> seq_node_notice {0};   // business version
        std::atomic<uint64_t> seq_snapshot {0};      // odd: writing, even: stable
        uint32 node_id = 0;
        uint32 node_notice_len = 0;
        uint32 node_notice_crc32 = 0;
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
    static uint32 Crc32(const char* data, size_t len)
    {
        uint32 crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i)
        {
            crc ^= static_cast<uint8_t>(data[i]);
            for (int j = 0; j < 8; ++j)
            {
                crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
            }
        }
        return ~crc;
    }

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
        if (data.empty() || data.size() >= sizeof(m_pShm->node_notice_blob))
        {
            return false;
        }
        // Begin write: seq_snapshot odd means "writing in progress".
        m_pShm->seq_snapshot.fetch_add(1, std::memory_order_release);
        memcpy(m_pShm->node_notice_blob, data.data(), data.size());
        m_pShm->node_notice_len = static_cast<uint32_t>(data.size());
        m_pShm->node_notice_crc32 = Crc32(m_pShm->node_notice_blob, data.size());
        m_pShm->seq_node_notice.fetch_add(1, std::memory_order_release);
        // End write: seq_snapshot even means "stable snapshot".
        m_pShm->seq_snapshot.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool GetNodeNotice(NodeNotice& oNodeNotice) const
    {
        if (!m_pShm)
        {
            return false;
        }
        // Double-snapshot read: read while snapshot version remains stable and even.
        for (int retry = 0; retry < 3; ++retry)
        {
            const uint64_t snapA = m_pShm->seq_snapshot.load(std::memory_order_acquire);
            if (snapA & 1u)
            {
                continue;  // writer in progress
            }
            const uint32 len = m_pShm->node_notice_len;
            const uint32 crc = m_pShm->node_notice_crc32;
            if (len == 0 || len >= static_cast<uint32>(sizeof(m_pShm->node_notice_blob)))
            {
                return false;
            }
            std::string data;
            data.resize(len);
            memcpy(&data[0], m_pShm->node_notice_blob, len);
            const uint64_t snapB = m_pShm->seq_snapshot.load(std::memory_order_acquire);
            if (snapA != snapB || (snapB & 1u))
            {
                continue;
            }
            if (Crc32(data.data(), data.size()) != crc)
            {
                return false;
            }
            return oNodeNotice.ParseFromArray(data.data(), static_cast<int>(data.size()));
        }
        return false;
    }

    void UpdateNodeNoticeVersion()
    {
        if (m_pShm)
        {
            m_ack.node_notice = m_pShm->seq_node_notice.load(std::memory_order_acquire);
        }
    }

    bool IsNodeNoticeVersionChange() const
    {
        return m_pShm && (m_pShm->seq_node_notice.load(std::memory_order_acquire) > m_ack.node_notice);
    }

    uint64 GetNodeNoticeVersion() const
    {
        return m_pShm ? static_cast<uint64>(m_pShm->seq_node_notice.load(std::memory_order_acquire)) : 0;
    }
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
