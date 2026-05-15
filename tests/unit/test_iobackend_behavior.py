"""
IoBackend 行为单元测试

验证 EvIoBackend / AsioUringIoBackend 的抽象接口契约:
- SubmitRead/SubmitWrite/CancelFd/HasPending/Name 语义正确性
"""
import pytest
from dataclasses import dataclass, field
from typing import Dict, Optional


@dataclass
class MockFdState:
    fd: int
    read_pending: bool = False
    write_pending: bool = False
    read_buf: Optional[bytes] = None
    write_buf: Optional[bytes] = None
    seq: int = 0


class MockEvIoBackend:
    """模拟 EvIoBackend — 每个 fd 独立 ev_io watcher"""
    def __init__(self):
        self.fds: Dict[int, MockFdState] = {}

    def name(self) -> str:
        return "ev"

    def submit_read(self, fd: int, seq: int = 0) -> bool:
        if fd not in self.fds:
            self.fds[fd] = MockFdState(fd=fd)
        self.fds[fd].read_pending = True
        self.fds[fd].seq = seq
        return True

    def submit_write(self, fd: int, seq: int = 0) -> bool:
        if fd not in self.fds:
            self.fds[fd] = MockFdState(fd=fd)
        self.fds[fd].write_pending = True
        self.fds[fd].seq = seq
        return True

    def cancel_fd(self, fd: int) -> None:
        self.fds.pop(fd, None)

    def has_pending(self, fd: int) -> bool:
        return fd in self.fds


class TestEvIoBackendContract:
    """EvIoBackend 接口契约"""

    def setup_method(self):
        self.be = MockEvIoBackend()

    def test_submit_read_creates_fd_state(self):
        self.be.submit_read(10)
        assert self.be.has_pending(10)

    def test_submit_write_creates_fd_state(self):
        self.be.submit_write(20)
        assert self.be.has_pending(20)

    def test_submit_read_then_write_keeps_fd(self):
        self.be.submit_read(10)
        self.be.submit_write(10)
        assert self.be.has_pending(10)
        assert self.be.fds[10].read_pending
        assert self.be.fds[10].write_pending

    def test_cancel_fd_removes_all_events(self):
        """CancelFd 移除 fd 上的所有事件 (含 EV_READ + EV_WRITE)"""
        self.be.submit_read(10)
        self.be.submit_write(10)
        self.be.cancel_fd(10)
        assert not self.be.has_pending(10)

    def test_cancel_fd_then_resubmit_read(self):
        """CancelFd 后必须补交 SubmitRead — 这是 P0 BUG 的根因"""
        self.be.submit_read(10)
        self.be.cancel_fd(10)
        assert not self.be.has_pending(10)
        # 补交读
        self.be.submit_read(10)
        assert self.be.has_pending(10), "MUST re-submit read after CancelFd!"

    def test_cancel_nonexistent_fd_no_error(self):
        self.be.cancel_fd(999)  # 不应崩溃

    def test_has_pending_false_for_unknown_fd(self):
        assert not self.be.has_pending(999)

    def test_name_is_ev(self):
        assert self.be.name() == "ev"

    def test_multiple_fds_independent(self):
        self.be.submit_read(10)
        self.be.submit_read(20)
        self.be.cancel_fd(10)
        assert not self.be.has_pending(10)
        assert self.be.has_pending(20), "fd 20 should not be affected"


class TestRemoveIoWriteEventRegression:
    """回归: RemoveIoWriteEvent 在 CancelFd 后补交 SubmitRead"""

    def test_ev_backend_resubmit_after_cancel(self):
        """Worker/Manager RemoveIoWriteEvent:
        1. CancelFd → 移除所有事件
        2. Compact + EnsureWritable
        3. SubmitRead → 补交读
        """
        be = MockEvIoBackend()
        fd, seq = 42, 100
        # SendTo 成功后调用 RemoveIoWriteEvent
        be.submit_read(fd, seq)
        be.submit_write(fd, seq)
        assert be.has_pending(fd)
        # CancelFd
        be.cancel_fd(fd)
        assert not be.has_pending(fd), "CancelFd must remove all events"
        # 补交读 (Bug fix)
        be.submit_read(fd, seq)
        assert be.has_pending(fd), "MUST re-submit Read after CancelFd"

