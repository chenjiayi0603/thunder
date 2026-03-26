/*******************************************************************************
* Project:  Thunder
* @file     CustomConfigVersionData.hpp
* @brief    Shared custom-config data between processes
******************************************************************************/
#ifndef SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_

#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"

struct CustomConfigVersionData
{
    struct CustomConfigVersionMM
    {
        uint64 seq_custom = 0;
        uint32 custom_len = 0;
        char custom_blob[160 * 1024] = {0};
    };

private:
    CustomConfigVersionMM* m_pShm = nullptr;
    uint64 m_ack_custom = 0;

public:
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
        // Write order: blob -> len -> version++
        memcpy(m_pShm->custom_blob, customContent.data(), customContent.size());
        m_pShm->custom_blob[customContent.size()] = '\0';
        m_pShm->custom_len = static_cast<uint32>(customContent.size());
        ++m_pShm->seq_custom;
        return true;
    }

    bool GetCustomConfig(std::string& customContent) const
    {
        if (!m_pShm || m_pShm->custom_len == 0 ||
            m_pShm->custom_len >= static_cast<uint32>(sizeof(m_pShm->custom_blob)))
        {
            return false;
        }
        customContent.assign(m_pShm->custom_blob, m_pShm->custom_len);
        return true;
    }

    bool IsCustomVersionChange() const
    {
        return m_pShm && (m_pShm->seq_custom > m_ack_custom);
    }

    void UpdateCustomVersion()
    {
        if (m_pShm)
        {
            m_ack_custom = m_pShm->seq_custom;
        }
    }

    uint64 GetCustomVersion() const
    {
        return m_pShm ? m_pShm->seq_custom : 0;
    }

    uint32 GetCustomLength() const
    {
        return m_pShm ? m_pShm->custom_len : 0;
    }
};

#endif /* SRC_LABOR_TYPES_CUSTOM_CONFIG_VERSION_DATA_HPP_ */
