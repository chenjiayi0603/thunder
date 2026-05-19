/*******************************************************************************
 * Project:  Net
 * @file     IoBackend.hpp
 * @brief    I/O backend abstraction (ev / io_uring / DPDK+mTCP) — 策略模式接口
 * @author   cjy
 * @date:    2026年5月11日
 * @note
 * Modify history:
 *   2026-05-18  v2.0  扩展为 12 方法完整策略接口，覆盖连接生命周期全流程，
 *                     新增 CreateListenSocket/Accept/CloseFd/SetSocketOpt/GetPeerName，
 *                     支撑一套 Worker 代码适配 ev/io_uring/DPDK+mTCP 三种后端。
 ******************************************************************************/
#ifndef IOBACKEND_HPP_
#define IOBACKEND_HPP_

#include <cstdint>
#include <memory>

struct ev_loop;

namespace util { class CBuffer; }

namespace net
{

/**
 * @brief I/O 操作类型
 */
enum class IoOp : uint8_t
{
    Read       = 0,
    Write      = 1,
    // send_zc 的第二个 CQE（IORING_CQE_F_NOTIF）：buffer 已脱离内核引用，可安全复用。
    // 仅原生 io_uring + send_zc 路径产生；ev / 普通 send / asio_uring 永不产生此事件。
    WriteNotif = 2,
};

/**
 * @brief 对端地址
 */
struct PeerAddr
{
    char     ip[64] = {};
    uint16_t port   = 0;
};

/**
 * @brief I/O 完成回调
 * @param fd       文件描述符
 * @param seq      序号（用于连接校验）
 * @param op       操作类型（读/写）
 * @param result   传输字节数（>0）、0（EOF 或 buffer 为空）、<0（错误，-result 为 errno）
 * @param user_data 用户数据
 */
using IoCompletionCallback = void (*)(int fd, uint32_t seq, IoOp op, int result, void* user_data);

/**
 * @brief I/O 后端抽象接口（策略模式）
 * @note
 *   一套 Worker 代码零分支适配 ev / io_uring / DPDK+mTCP 三种底层。
 *   Worker 永远通过 IoBackend 指针调用，不直接碰 socket API。
 *
 *   角色关系:
 *     Worker (业务) → IoBackend (策略接口)
 *           → EvIoBackend      — libev epoll（内核 fd）
 *           → AsioUringIoBackend — io_uring（内核 fd，I/O 走 uring）
 *           → DpdkIoBackend    — DPDK+mTCP（用户态 sockid）
 */
class IoBackend
{
public:
    virtual ~IoBackend() = default;

    // ========== 生命周期 ==========

    /** @brief 初始化后端，绑定 libev loop、完成回调和用户数据 */
    virtual bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) = 0;

    /** @brief 销毁后端，释放所有资源 */
    virtual void Destroy() = 0;

    /** @brief 后端名称（如 "ev", "asio_uring", "dpdk"） */
    virtual const char* Name() const = 0;

    // ========== 监听 socket 创建 ==========

    /**
     * @brief 创建监听 socket
     * @param ip           监听 IP
     * @param port         监听端口
     * @param boReusePort  是否设置 SO_REUSEPORT（mTCP 不支持时静默忽略）
     * @param backlog      listen backlog
     * @return fd（内核 fd 或 mTCP sockid），失败返回 -1
     */
    virtual int  CreateListenSocket(const char* ip, uint16_t port,
                                    bool boReusePort, int backlog) = 0;

    // ========== Accept ==========

    /**
     * @brief 从监听 fd accept 新连接
     * @param listenFd    监听 fd
     * @param outPeerAddr 输出对端地址
     * @return client fd，<0 表示暂无连接（EAGAIN）或错误
     */
    virtual int  Accept(int listenFd, PeerAddr& outPeerAddr) = 0;

    // ========== I/O 提交 ==========

    /** @brief 提交读操作；buf 为 shared_ptr，后端持有引用以保护零拷贝生命周期 */
    virtual bool SubmitRead(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) = 0;

    /** @brief 提交写操作；buf 为 shared_ptr，后端持有引用以保护零拷贝生命周期 */
    virtual bool SubmitWrite(int fd, std::shared_ptr<util::CBuffer> buf, uint32_t seq) = 0;

    // ========== 连接关闭 ==========

    /** @brief 取消 fd 上所有待处理的 I/O 注册（连接关闭前先调） */
    virtual void CancelFd(int fd) = 0;

    /** @brief 实际关闭连接（内核 close 或 mtcp_close） */
    virtual void CloseFd(int fd) = 0;

    /** @brief 是否有待处理的 I/O（用于安全关闭校验） */
    virtual bool HasPending(int fd) const = 0;

    // ========== Socket 属性 ==========

    /**
     * @brief 设置连接属性（TCP_NODELAY, SO_KEEPALIVE 等）。
     *        mTCP 不支持的选项静默忽略（不报错）。
     */
    virtual void SetSocketOpt(int fd) = 0;

    // ========== 对端地址 ==========

    /**
     * @brief 获取对端地址。
     *        调用方应优先使用 Accept 时缓存的地址；
     *        此方法作为兜底（ev/uring 用 getpeername，dpdk 用缓存的地址返回）。
     */
    virtual bool GetPeerName(int fd, PeerAddr& outAddr) = 0;
};

} /* namespace net */

#endif /* IOBACKEND_HPP_ */
