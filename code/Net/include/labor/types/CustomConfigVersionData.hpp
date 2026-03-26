/*******************************************************************************
* Project:  Thunder
* @file     CustomConfigVersionData.hpp
* @brief    Shared custom-config data between processes
******************************************************************************/
#ifndef SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"

struct CustomConfigVersionData
{
    struct CustomConfigVersionMM
    {
        std::atomic<uint64_t> seq_custom {0};    // business version
        std::atomic<uint64_t> seq_snapshot {0};  // odd: writing, even: stable
        uint32 custom_len = 0;
        uint32 custom_crc32 = 0;
        char custom_blob[160 * 1024] = {0};
    };

private:
    CustomConfigVersionMM* m_pShm = nullptr;
    uint64 m_ack_custom = 0;

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

    void SetCustomConfigVersionMM(CustomConfigVersionMM* customConfigVersionMM = nullptr)
    {
        if (customConfigVersionMM)
        {
            DelCustomConfigVersionMM();
            m_pShm = customConfigVersionMM;
        }
        else if (m_pShm == nullptr)
        {
            m_pShm = static_cast<CustomConfigVersionMM*>(mmap(
                NULL, sizeof(CustomConfigVersionMM), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
            memset(m_pShm, 0, sizeof(*m_pShm));
        }
    }

    CustomConfigVersionMM* GetCustomConfigVersionMM()
    {
        if (m_pShm == nullptr)
        {
            SetCustomConfigVersionMM();
        }
        return m_pShm;
    }

    void DelCustomConfigVersionMM()
    {
        if (m_pShm)
        {
            munmap(m_pShm, sizeof(*m_pShm));
            m_pShm = nullptr;
        }
    }

    bool SetCustomConfig(const std::string& customContent)
    {
        if (!m_pShm || customContent.empty())
        {
            return false;
        }
        if (customContent.size() >= sizeof(m_pShm->custom_blob))
        {
            return false;
        }
        // Write order: snapshot(start) -> blob -> len/crc -> version++ -> snapshot(end)
        m_pShm->seq_snapshot.fetch_add(1, std::memory_order_release);
        memcpy(m_pShm->custom_blob, customContent.data(), customContent.size());
        m_pShm->custom_blob[customContent.size()] = '\0';
        m_pShm->custom_len = static_cast<uint32>(customContent.size());
        m_pShm->custom_crc32 = Crc32(m_pShm->custom_blob, customContent.size());
        m_pShm->seq_custom.fetch_add(1, std::memory_order_release);
        m_pShm->seq_snapshot.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool GetCustomConfig(std::string& customContent) const
    {
        if (!m_pShm)
        {
            return false;
        }
        for (int retry = 0; retry < 3; ++retry)
        {
            const uint64_t snapA = m_pShm->seq_snapshot.load(std::memory_order_acquire);
            if (snapA & 1u)
            {
                continue;
            }
            const uint32 len = m_pShm->custom_len;
            const uint32 crc = m_pShm->custom_crc32;
            if (len == 0 || len >= static_cast<uint32>(sizeof(m_pShm->custom_blob)))
            {
                return false;
            }
            std::string data;
            data.resize(len);
            memcpy(&data[0], m_pShm->custom_blob, len);
            const uint64_t snapB = m_pShm->seq_snapshot.load(std::memory_order_acquire);
            if (snapA != snapB || (snapB & 1u))
            {
                continue;
            }
            if (Crc32(data.data(), data.size()) != crc)
            {
                return false;
            }
            customContent = std::move(data);
            return true;
        }
        return false;
    }

    bool IsCustomVersionChange() const
    {
        return m_pShm && (m_pShm->seq_custom.load(std::memory_order_acquire) > m_ack_custom);
    }

    void UpdateCustomVersion()
    {
        if (m_pShm)
        {
            m_ack_custom = m_pShm->seq_custom.load(std::memory_order_acquire);
        }
    }

    uint64 GetCustomVersion() const
    {
        return m_pShm ? static_cast<uint64>(m_pShm->seq_custom.load(std::memory_order_acquire)) : 0;
    }

    uint32 GetCustomLength() const
    {
        return m_pShm ? m_pShm->custom_len : 0;
    }
};

#endif /* SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_ */
