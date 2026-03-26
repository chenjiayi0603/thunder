/*******************************************************************************
* Project:  Thunder
* @file     LoaderConfigVersionData.hpp
* @brief    Shared version data between Manager/Loader/Worker
******************************************************************************/
#ifndef SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_

#include <algorithm>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"

/** Manager/Loader/Worker 共用 MAP_SHARED；shm.seq_* 为跨进程事件序号，m_ack 为本进程已消费序号 */
struct LoaderConfigVersionData
{
    struct LoaderConfigVersionMM
    {
        uint64 seq_config = 0;
        uint64 seq_restart_workers = 0;
        char server_config_name[64] = {0};
        char server_config_body[16 * 1024] = {0};
    };

private:
    LoaderConfigVersionMM* m_pShm = nullptr;
    struct
    {
        uint64 config = 0;
        uint64 restart_workers = 0;
    } m_ack{};

public:
    bool m_bLoaderProcess = false;

    void SetLoaderConfigVersionMM(LoaderConfigVersionMM* loaderConfigVersionMM = nullptr)
    {
        if (loaderConfigVersionMM)
        {
            DelLoaderConfigVersionMM();
            m_pShm = loaderConfigVersionMM;
        }
        else if (m_pShm == nullptr)
        {
            m_pShm = static_cast<LoaderConfigVersionMM*>(mmap(
                NULL, sizeof(LoaderConfigVersionMM), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
            memset(m_pShm, 0, sizeof(*m_pShm));
        }
    }

    LoaderConfigVersionMM* GetLoaderConfigVersionMM()
    {
        if (m_pShm == nullptr)
        {
            SetLoaderConfigVersionMM();
        }
        return m_pShm;
    }

    void DelLoaderConfigVersionMM()
    {
        if (m_pShm)
        {
            munmap(m_pShm, sizeof(*m_pShm));
            m_pShm = nullptr;
        }
    }

    bool IsLoaderProcess() const { return m_bLoaderProcess; }
    uint64 IncLoaderConfigVersion() { return m_pShm ? ++m_pShm->seq_config : 0; }
    bool IsConfigVersionChange() const { return m_pShm && (m_pShm->seq_config > m_ack.config); }

    void UpdateLoaderConfigVersion()
    {
        if (m_pShm)
        {
            m_ack.config = m_pShm->seq_config;
        }
    }

    void SetServerConfigFile(const std::string& configName, const std::string& configContent)
    {
        if (!m_pShm || configName.empty() || configContent.empty())
        {
            return;
        }
        const size_t nameCap = sizeof(m_pShm->server_config_name);
        const size_t nameLen = std::min(configName.size(), nameCap - 1);
        memcpy(m_pShm->server_config_name, configName.c_str(), nameLen);
        m_pShm->server_config_name[nameLen] = '\0';

        const size_t bodyCap = sizeof(m_pShm->server_config_body);
        const size_t bodyLen = std::min(configContent.size(), bodyCap - 1);
        memcpy(m_pShm->server_config_body, configContent.c_str(), bodyLen);
        m_pShm->server_config_body[bodyLen] = '\0';
    }

    bool GetServerConfigFile(std::string& configContent) const
    {
        if (m_pShm && m_pShm->server_config_body[0] != 0)
        {
            configContent = m_pShm->server_config_body;
            return true;
        }
        return false;
    }

    void IncRestartWorkerOnUpdateConfigVersion()
    {
        if (m_pShm)
        {
            ++m_pShm->seq_restart_workers;
        }
    }

    void UpdateRestartWorkerOnUpdateConfigVersion()
    {
        if (m_pShm)
        {
            m_ack.restart_workers = m_pShm->seq_restart_workers;
        }
    }

    bool IsRestartWorkerOnUpdateConfigChange() const
    {
        return m_pShm && (m_pShm->seq_restart_workers > m_ack.restart_workers);
    }
};

#endif /* SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_ */
