/*******************************************************************************
* Project:  Thunder
* @file     LoaderConfigVersionData.hpp
* @brief    Shared-memory config version synchronizer for Manager↔Loader↔Worker
* @author   cjy
* @date:    2026-05-24
* @note
*   设计要点:
*     1. 跨进程配置同步 — Loader 拉取配置写入 SHM，Manager/Worker 轮询消费
*     2. 版本计数器 — seq_config 为单调递增非零序列号，m_consumedSeq 为本进程已
*        消费版本号，比较二者判断是否有新配置
*     3. 共享内存 (MAP_SHARED | MAP_ANON) — fork() 后父子进程自然共享，无需文件
*     4. 双层结构 — LoaderConfigVersionMM (共享区) + LoaderConfigVersionData (访问器)
*     5. 单一生产者 (Loader) / 多消费者 (Manager + N×Worker) — 生产者独占写
*        seq_config 和 config body，消费者只读；无锁，依赖原子变量 acquire/release
*
*   数据流:
*     Loader:  读本地/远端配置 → SetServerConfigFile() → IncLoaderConfigVersion()
*     Manager: fork() 前创建 SHM → fork Loader → poll IsConfigVersionChange()
*     Worker:  CheckShareMem() → IsConfigVersionChange() → GetServerConfigFile()
*
*   并发协议:
*     - 生产者: SetServerConfigFile() 写 body → IncLoaderConfigVersion() (release)
*     - 消费者: IsConfigVersionChange() (acquire) → 读 body
*     - 消费者只检查 seq_config > m_consumedSeq，消息顺序以 seq 单调性保证
*     - 不支持 seq 回绕: uint64 足以覆盖进程生命周期 (~584 年 @ 1e9/s)
*
* Modify history:
*   2026-05-24  补充设计注释与并发协议文档
******************************************************************************/
#ifndef SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_
#define SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include "NetDefine.hpp"

// ═══════════════════════════════════════════════════════════════════════════════
// LoaderConfigVersionData — 跨进程配置版本同步器
//
// 角色:
//   - Loader (子进程): 配置生产者，负责从文件/网络加载配置并写入 SHM
//   - Manager (父进程): 配置消费者，fork Loader 后轮询是否有新配置
//   - Worker (子进程): 配置消费者，在 CheckShareMem() 中异步检测并应用配置
//
// 为什么需要这个结构:
//   Thunder 集群中，Manager 负责编排子进程 (Loader / Worker)，但配置热更新
//   不是通过 IPC 消息通道 (ShmRingQueue) 传递，而是通过共享内存版本计数器实现:
//     1. Loader 将配置直接写入 SHM 的 config body 缓冲区
//     2. Loader 递增 seq_config 版本号，"发布"新配置
//     3. Manager / Worker 各自轮询 seq_config，发现版本号变化后读取 config body
//   这种设计的优势: 多消费者无需遍历通知，一次写入各进程自行感知。
//
// Note:
//   所有 <<Manager/Loader/Worker>> 在 fork() 前继承父进程的 SHM 映射，因此
//   它们共享同一个物理页。进程内各自创建独立实例，但 m_pShm 指向同一块内存。
// ═══════════════════════════════════════════════════════════════════════════════
struct LoaderConfigVersionData
{
    // ═══════════════════════════════════════════════════════════════════════
    // 共享内存数据区 — 这部分数据在 fork() 后所有进程可见
    //
    // 存放在 MAP_SHARED | MAP_ANON 区域中，内容跨进程实时同步。
    // seq_config 使用 std::atomic，保证跨核/跨进程的可见性和修改顺序。
    // server_config_name/body 为纯 char 数组，由生产者独占写入，
    // 消费者在检测到 seq_config 变化后才读取。
    //
    // 容量: name 最大 63 字节 (含 '\0')，body 最大 ~16KB。
    // 为何用固定大小而不动态分配: SHM 不支持堆分配器，且固定大小
    // 便于 mmap 一次性映射，避免跨进程指针传递的复杂性。
    // ═══════════════════════════════════════════════════════════════════════
    struct LoaderConfigVersionMM
    {
        std::atomic<uint64_t> seq_config {0};          // 配置版本号 (单调递增，0 = 初始/未发布)
        char server_config_name[64] = {0};              // 配置文件名 (如 "Hello.json")
        char server_config_body[16 * 1024] = {0};       // 配置文件内容 (JSON 字符串)
    };

private:
    // ── 指向共享内存区域的指针 ──
    // m_pShm 可能指向:
    //   a) 本进程 mmap 创建的新区域 (Manager 首次调用 SetLoaderConfigVersionMM())
    //   b) 从外部传入的已有区域  (Loader 构造函数接收 Manager 的 SHM 指针)
    //   c) nullptr — 未初始化 (Loader 进程禁用时)
    LoaderConfigVersionMM* m_pShm = nullptr;

    // ── 本进程已消费的版本号 (私有，非共享) ──
    // 不同进程各自维护自己的 m_consumedSeq，用于与 SHM 中的 seq_config 对比。
    // 当 seq_config > m_consumedSeq.config 时表示有新配置待消费。
    struct
    {
        uint64 config = 0;
    } m_consumedSeq{};

public:
    // ── 是否运行在 Loader 进程中 ──
    // Manager 在 CreateLoader() 中设置此标志，Manager/Worker 实例该值为 false。
    // 用于条件判断: Manager::LoadConf() 仅当 IsLoaderProcess() 为 true 时才
    // 检查共享内存版本变更 (因为只有 Loader 存在时 SHM 才会更新)。
    bool m_bLoaderProcess = false;

    // ═══════════════════════════════════════════════════════════════════════
    // SHM 生命周期管理
    // ═══════════════════════════════════════════════════════════════════════

    // ── 设置/创建共享内存区域 ──
    // 两种调用模式:
    //   1. 传入已有指针: Manager fork() Loader 后，子进程继承父进程 SHM 映射，
    //      但 Loader 构造函数通过此方法显式接管指针所有权
    //   2. 无参调用 (默认): 进程首次访问时自动 mmap 创建新区域
    //      使用 MAP_SHARED | MAP_ANON: 匿名共享映射，fork() 后父子进程自动共享
    void SetLoaderConfigVersionMM(LoaderConfigVersionMM* loaderConfigVersionMM = nullptr)
    {
        if (loaderConfigVersionMM)
        {
            DelLoaderConfigVersionMM();          // 先释放旧区域 (如有)
            m_pShm = loaderConfigVersionMM;      // 接管外部指针
        }
        else if (m_pShm == nullptr)
        {
            // 惰性创建: 首次访问时分配，避免未启用 Loader 时浪费内存
            m_pShm = static_cast<LoaderConfigVersionMM*>(mmap(
                NULL, sizeof(LoaderConfigVersionMM), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
            // placement new: 在 mmap 区域上构造原子变量 (初始化为 0)
            // 不分配新内存，仅调用构造函数初始化 std::atomic 对象
            new (m_pShm) LoaderConfigVersionMM();
        }
    }

    // ── 获取共享内存指针 (确保已初始化) ──
    // 首次调用时自动触发惰性分配 (mmap + placement new)
    LoaderConfigVersionMM* GetLoaderConfigVersionMM()
    {
        if (m_pShm == nullptr)
        {
            SetLoaderConfigVersionMM();
        }
        return m_pShm;
    }

    // ── 释放共享内存区域 ──
    // 注意: MAP_SHARED 区域的 munmap 仅解除当前进程的映射，不影响其他进程。
    // 物理页在所有映射者 munmap 后由内核回收。
    void DelLoaderConfigVersionMM()
    {
        if (m_pShm)
        {
            munmap(m_pShm, sizeof(*m_pShm));
            m_pShm = nullptr;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 版本控制 API
    //   - 生产者 (Loader): IncLoaderConfigVersion() — 发布新配置版本
    //   - 消费者 (Manager/Worker): IsConfigVersionChange() + UpdateLoaderConfigVersion()
    // ═══════════════════════════════════════════════════════════════════════

    bool IsLoaderProcess() const { return m_bLoaderProcess; }

    // ── 递增配置版本号 (生产者 — Loader 调用) ──
    // fetch_add(1, release):
    //   - release 保证 SetServerConfigFile() 的所有写入 (config body) 在
    //     此操作之前对其他核心可见
    //   - 返回旧值 + 1 = 新版本号
    //   - 首次调用返回 1 (seq_config 初始为 0)
    uint64 IncLoaderConfigVersion()
    {
        if (!m_pShm)
        {
            return 0;
        }
        return static_cast<uint64>(m_pShm->seq_config.fetch_add(1, std::memory_order_release) + 1);
    }

    // ── 检查是否有新配置 (消费者 — Manager/Worker 调用) ──
    // load(acquire):
    //   - acquire 与生产者的 release 配对，保证读取 seq_config 时
    //     能观察到 SetServerConfigFile() 已写入的完整 config body
    //   - 比较 SHM 版本号 vs 本进程已消费版本号
    //   - 返回 true 表示有新配置待读取
    bool IsConfigVersionChange() const
    {
        return m_pShm
                && (static_cast<uint64>(m_pShm->seq_config.load(std::memory_order_acquire)) > m_consumedSeq.config);
    }

    // ── 标记新配置已消费 (消费者调用) ──
    // 将本进程 m_consumedSeq 同步到 SHM 当前 seq_config，表示"我已处理完此版本"。
    // 这不会修改 SHM 数据，只更新本地状态，各进程独立调用。
    void UpdateLoaderConfigVersion()
    {
        if (m_pShm)
        {
            m_consumedSeq.config = static_cast<uint64>(m_pShm->seq_config.load(std::memory_order_acquire));
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 配置内容存取 — server_config_name/body 在 SHM 中
    //
    // 注意: SetServerConfigFile 必须在 IncLoaderConfigVersion 之前调用，
    // 以利用 release fence 保证 body 写入对消费者可见。
    // 消费者必须在 IsConfigVersionChange() 返回 true 之后调用
    // GetServerConfigFile()，以利用 acquire fence 读取完整 body。
    // ═══════════════════════════════════════════════════════════════════════

    // ── 写入配置内容到 SHM (生产者 — Loader 调用) ──
    // 输入: configName (如 "Hello.json"), configContent (JSON 字符串)
    // 容量限制: name ≤ 63B, body ≤ ~16KB (截断超出部分)
    void SetServerConfigFile(const std::string& configName, const std::string& configContent)
    {
        if (!m_pShm || configName.empty() || configContent.empty())
        {
            return;
        }
        // 复制 config name，留 '\0' 终止位
        const size_t nameCap = sizeof(m_pShm->server_config_name);
        const size_t nameLen = std::min(configName.size(), nameCap - 1);
        memcpy(m_pShm->server_config_name, configName.c_str(), nameLen);
        m_pShm->server_config_name[nameLen] = '\0';

        // 复制 config body，留 '\0' 终止位
        const size_t bodyCap = sizeof(m_pShm->server_config_body);
        const size_t bodyLen = std::min(configContent.size(), bodyCap - 1);
        memcpy(m_pShm->server_config_body, configContent.c_str(), bodyLen);
        m_pShm->server_config_body[bodyLen] = '\0';
    }

    // ── 从 SHM 读取配置内容 (消费者 — Manager/Worker 调用) ──
    // 返回 true 表示 SHM 中存在有效配置 (body 首字符非 '\0')
    bool GetServerConfigFile(std::string& configContent) const
    {
        if (m_pShm && m_pShm->server_config_body[0] != 0)
        {
            configContent = m_pShm->server_config_body;
            return true;
        }
        return false;
    }

};

#endif /* SRC_LABOR_TYPES_LOADER_CONFIG_VERSION_DATA_HPP_ */
