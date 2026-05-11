/*******************************************************************************
 * Project:  Net
 * @file     IoBackend.hpp
 * @brief    I/O backend abstraction (ev / io_uring)
 * @author   cjy
 * @date:    2026年5月11日
 * @note
 * Modify history:
 ******************************************************************************/
#ifndef IOBACKEND_HPP_
#define IOBACKEND_HPP_

#include <cstdint>

struct ev_loop;

namespace util { class CBuffer; }

namespace net
{

/**
 * @brief I/O 操作类型
 */
enum class IoOp : uint8_t
{
    Read  = 0,
    Write = 1,
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
 * @brief I/O 后端抽象接口
 * @note  统一 ev_io（libev）和 io_uring 两种异步 I/O 方式
 */
class IoBackend
{
public:
    virtual ~IoBackend() = default;

    /** @brief 初始化后端，绑定 libev loop、完成回调和用户数据 */
    virtual bool Init(struct ev_loop* loop, IoCompletionCallback callback, void* user_data) = 0;

    /** @brief 销毁后端，释放所有资源 */
    virtual void Destroy() = 0;

    /** @brief 提交读操作；数据到达后通过回调通知 */
    virtual bool SubmitRead(int fd, util::CBuffer* buf, uint32_t seq) = 0;

    /** @brief 提交写操作；写完成后通过回调通知 */
    virtual bool SubmitWrite(int fd, util::CBuffer* buf, uint32_t seq) = 0;

    /** @brief 取消 fd 上所有待处理/已注册的 I/O 操作（连接关闭前调用） */
    virtual void CancelFd(int fd) = 0;

    /** @brief 后端名称（如 "ev", "uring"） */
    virtual const char* Name() const = 0;

    /** @brief 是否有待处理的 I/O（用于安全关闭） */
    virtual bool HasPending(int fd) const = 0;
};

} /* namespace net */

#endif /* IOBACKEND_HPP_ */
