/*******************************************************************************
 * Project:  Net
 * @file     DpdkIoBackend.hpp
 * @brief    DPDK + mTCP 用户态 TCP 后端 — 策略模式具体实现
 *
 * === 架构 ===
 *
 *   Worker  ──→ IoBackend (策略接口) ──→ DpdkIoBackend
 *                                              │
 *                          ┌────────────────────┤
 *                          │  mTCP API          │  libev ev_io
 *                          │  (用户态 sockid)    │  (事件通知)
 *                          ▼                    ▼
 *                    mTCP 用户态 TCP 栈      libev 主循环
 *                          │
 *                          ▼
 *                    DPDK PMD (轮询模式驱动)
 *                          │
 *                          ▼
 *                    物理网卡 (NIC)
 *
 * === sockid 隔离 ===
 *
 * mTCP sockid 与内核 fd 可能冲突。偏移 10,000,000 避免命名空间重叠。
 * 偏移量由常量 kDpdkSockIdOffset 定义。
 *
 * === 使用条件 ===
 *
 * 1. 物理机（非云虚拟机）
 * 2. DPDK 兼容网卡（Intel X710/X520/82599, Mellanox CX4+, etc.）
 * 3. 内核启动参数: hugepages, iommu=pt
 * 4. mTCP 编译并链接
 * 5. 配置 "io_backend": "dpdk"
 *
 * === 编译 ===
 *   cmake -DENABLE_DPDK=ON  (定义 THUNDER_IO_DPDK)
 *
 * @warning  当前为骨架实现: 无 DPDK/mTCP 库时编译通过但不执行真实 DPDK I/O。
 *           生产使用需安装 DPDK + mTCP SDK 并编译为 HAVE_DPDK=1。
 *
 * @see      IoBackend, EvIoBackend
 * @see      docs/uring/DPDK+mTCP设计文档.md
 ******************************************************************************/
#ifndef DPDKIOBACKEND_HPP_
#define DPDKIOBACKEND_HPP_

#include "labor/IoBackend.hpp"
#include "libev/ev.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace util { class CBuffer; }

namespace net
{

/// mTCP sockid 偏移量 — 避免与内核 fd 命名空间冲突
constexpr int kDpdkSockIdOffset = 10000000;

class DpdkIoBackend : public IoBackend
{
public:
    DpdkIoBackend();
    ~DpdkIoBackend() override;

    // ========== 生命周期 ==========
    bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) override;
    void Destroy() override;
    const char* Name() const override { return "dpdk"; }

    // ========== 监听 socket ==========
    int  CreateListenSocket(const char* ip, uint16_t port, bool boReusePort, int backlog) override;
    int  Accept(int listenFd, PeerAddr& outPeerAddr) override;

    // ========== I/O 提交 ==========
    bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;
    bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) override;

    // ========== 连接关闭 ==========
    void CancelFd(int fd) override;
    void CloseFd(int fd) override;
    bool HasPending(int fd) const override;

    // ========== Socket 属性 / 对端地址 ==========
    void SetSocketOpt(int fd) override;
    bool GetPeerName(int fd, PeerAddr& outAddr) override;

private:
    /// Accept 时缓存的 fd→对端地址映射（mTCP 无 getpeername）
    std::unordered_map<int, PeerAddr> m_mapPeerAddr;

    struct ev_loop*        m_loop     = nullptr;
    IoCompletionCallback   m_callback = nullptr;
    void*                  m_userData = nullptr;
    bool                   m_initialized = false;
};

} /* namespace net */

#endif /* DPDKIOBACKEND_HPP_ */
