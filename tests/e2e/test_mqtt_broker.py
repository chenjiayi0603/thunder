"""
MQTT Broker E2E 测试 — 覆盖 MQTT 3.1.1 核心协议流程

测试项:
  1. CONNECT/CONNACK        — 客户端建连
  2. SUBSCRIBE/SUBACK       — 订阅主题
  3. PUBLISH (QoS 0)       — 发布消息 + 广播给订阅者
  4. PUBLISH (QoS 1)       — 发布消息 + PUBACK 确认
  5. UNSUBSCRIBE/UNSUBACK  — 取消订阅
  6. PINGREQ/PINGRESP      — 心跳保活
  7. Echo Demo             — echo/+ → echo/+/response
  8. Retain 消息           — 保留消息投递给新订阅者
  9. Will 遗嘱消息          — 异常断连触发遗嘱 (待实现)

前置: deploy/MqttBroker 已启动 (access_port=21883, codec=CODEC_MQTT)
  本地模式: pytest --mode=local   (conftest 自动 docker compose up)
  外部模式: pytest --mode=external (自行启动 Broker, 只等端口就绪)
依赖: pip install paho-mqtt

运行: python3 -m pytest tests/e2e/test_mqtt_broker.py -v --mode=external -s
"""
from __future__ import annotations

import os
import threading
import time

import pytest
import paho.mqtt.client as mqtt

_HOST = os.getenv("E2E_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("THUNDER_MQTT_PORT", "21883"))
MQTT_HOST = (_HOST, MQTT_PORT)


# ═══════════════════════════════════════════════════════════════════════════
# 覆盖 conftest 的 service_readiness: MQTT 测试只需 21883 端口,
# 不依赖 HTTP/HTTPS/Interface/Center 等 Thunder 集群端口
# ═══════════════════════════════════════════════════════════════════════════
@pytest.fixture(scope="session", autouse=True)
def service_readiness(mode: str, docker_stack: object) -> None:
    """覆盖 conftest 的 service_readiness: 只等 MQTT 端口就绪"""
    del docker_stack
    if mode == "external":
        print("[mqtt test] 等待 MQTT Broker 端口 21883 就绪...", flush=True)
        import socket
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", MQTT_PORT), timeout=2):
                    print("[mqtt test] MQTT Broker 端口就绪", flush=True)
                    break
            except OSError:
                time.sleep(1)
        else:
            raise AssertionError(f"MQTT Broker port {MQTT_PORT} not ready within 30s")
    yield

# ── 工具函数 ──────────────────────────────────────────────────


def _make_client(client_id: str = "", clean_session: bool = True) -> mqtt.Client:
    """
    创建并连接一个 MQTT 客户端 (paho v2 API)

    paho v2 的 connect() 是异步的: 只发起 TCP 连接, CONNECT/CONNACK 在后台完成。
    因此需要 on_connect 回调 + connected Event + loop_start() 三者配合:
      - loop_start() 启动后台线程处理网络 I/O
      - on_connect 回调在收到 CONNACK 后 set event
      - connected.wait(timeout=5) 阻塞等待 CONNACK
    """
    connected = threading.Event()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, clean_session=clean_session)
    c.on_connect = lambda _c, _u, _f, rc, *a: connected.set() if rc == 0 else None
    c.connect(*MQTT_HOST, keepalive=10)
    c.loop_start()
    assert connected.wait(timeout=5), f"MQTT 连接超时 (client_id={client_id})"
    return c


def _wait_for(predicate, timeout: float = 5.0, interval: float = 0.1) -> bool:
    """轮询等待条件满足"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


# ── 测试类 ──────────────────────────────────────────────────


@pytest.mark.integration
@pytest.mark.smoke
class TestMqttConnect:
    """CONNECT / CONNACK 测试"""

    def test_connect_accepted(self) -> None:
        """正常连接应收到 CONNACK ACCEPTED"""
        c = _make_client(client_id="test_conn_01")
        assert c.is_connected()
        c.disconnect()
        c.loop_stop()

    def test_connect_multiple_clients(self) -> None:
        """多个客户端可同时连接"""
        clients = []
        for i in range(3):
            c = _make_client(client_id=f"test_multi_{i}")
            assert c.is_connected()
            clients.append(c)
        for c in clients:
            c.disconnect()
            c.loop_stop()


@pytest.mark.integration
@pytest.mark.smoke
class TestMqttSubPub:
    """SUBSCRIBE / PUBLISH QoS 0 测试"""

    def test_subscribe_and_publish_qos0(self) -> None:
        """订阅后发布 QoS 0 消息, 订阅者应收到"""
        received: list[tuple[str, bytes]] = []
        event = threading.Event()

        sub = _make_client(client_id="test_sub_qos0")
        sub.on_message = lambda _c, _u, msg: (received.append((msg.topic, msg.payload)), event.set())
        sub.subscribe("test/qos0", qos=0)
        time.sleep(0.3)  # 等 SUBACK

        pub = _make_client(client_id="test_pub_qos0")
        pub.publish("test/qos0", b"hello_qos0", qos=0)

        assert event.wait(timeout=5), "未收到 QoS 0 消息"
        assert len(received) == 1
        assert received[0][0] == "test/qos0"
        assert received[0][1] == b"hello_qos0"

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()

    def test_publish_to_multiple_subscribers(self) -> None:
        """一条消息应广播给所有订阅者"""
        received_a: list[bytes] = []
        received_b: list[bytes] = []
        event_a = threading.Event()
        event_b = threading.Event()

        sub_a = _make_client(client_id="test_bcast_a")
        sub_a.on_message = lambda _c, _u, msg: (received_a.append(msg.payload), event_a.set())
        sub_a.subscribe("test/broadcast", qos=0)

        sub_b = _make_client(client_id="test_bcast_b")
        sub_b.on_message = lambda _c, _u, msg: (received_b.append(msg.payload), event_b.set())
        sub_b.subscribe("test/broadcast", qos=0)
        time.sleep(0.3)

        pub = _make_client(client_id="test_bcast_pub")
        pub.publish("test/broadcast", b"broadcast_msg", qos=0)

        assert event_a.wait(timeout=5), "订阅者 A 未收到"
        assert event_b.wait(timeout=5), "订阅者 B 未收到"
        assert received_a[0] == b"broadcast_msg"
        assert received_b[0] == b"broadcast_msg"

        for c in (pub, sub_a, sub_b):
            c.loop_stop()
            c.disconnect()


@pytest.mark.integration
class TestMqttQos1:
    """PUBLISH QoS 1 + PUBACK 测试"""

    def test_publish_qos1_receives_puback(self) -> None:
        """QoS 1 发布应收到 PUBACK"""
        puback_received = threading.Event()

        pub = _make_client(client_id="test_qos1_pub")
        pub.on_publish = lambda _c, _u, mid, *a: puback_received.set()

        info = pub.publish("test/qos1", b"qos1_payload", qos=1)
        assert puback_received.wait(timeout=5), "未收到 PUBACK"

        pub.loop_stop()
        pub.disconnect()

    def test_subscribe_qos1_and_publish(self) -> None:
        """QoS 1 订阅 + QoS 1 发布"""
        received: list[bytes] = []
        event = threading.Event()

        sub = _make_client(client_id="test_qos1_sub")
        sub.on_message = lambda _c, _u, msg: (received.append(msg.payload), event.set())
        sub.subscribe("test/qos1sub", qos=1)
        time.sleep(0.3)

        pub = _make_client(client_id="test_qos1_pub2")
        pub.publish("test/qos1sub", b"qos1_msg", qos=1)

        assert event.wait(timeout=5), "未收到 QoS 1 消息"
        assert received[0] == b"qos1_msg"

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()


@pytest.mark.integration
class TestMqttUnsubscribe:
    """UNSUBSCRIBE / UNSUBACK 测试"""

    def test_unsubscribe_stops_messages(self) -> None:
        """取消订阅后不应再收到消息"""
        received: list[bytes] = []
        event = threading.Event()

        sub = _make_client(client_id="test_unsub")
        sub.on_message = lambda _c, _u, msg: (received.append(msg.payload), event.set())
        sub.subscribe("test/unsub", qos=0)
        time.sleep(0.3)

        # 发布第一条 — 应收到
        pub = _make_client(client_id="test_unsub_pub")
        pub.publish("test/unsub", b"before_unsub", qos=0)
        assert event.wait(timeout=5), "取消订阅前应收到消息"
        assert len(received) == 1

        # 取消订阅
        sub.unsubscribe("test/unsub")
        time.sleep(0.3)

        # 发布第二条 — 不应收到
        event.clear()
        pub.publish("test/unsub", b"after_unsub", qos=0)
        assert not event.wait(timeout=2), "取消订阅后不应收到消息"
        assert len(received) == 1

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()


@pytest.mark.integration
@pytest.mark.smoke
class TestMqttPing:
    """PINGREQ / PINGRESP 测试"""

    def test_ping_keepalive(self) -> None:
        """心跳应保持连接活跃"""
        c = _make_client(client_id="test_ping")
        assert c.is_connected()
        time.sleep(1)
        assert c.is_connected(), "心跳后连接应仍活跃"
        c.loop_stop()
        c.disconnect()


@pytest.mark.integration
@pytest.mark.smoke
class TestMqttEcho:
    """Echo Demo 测试 (echo/+ → echo/+/response)"""

    def test_echo_response(self) -> None:
        """发布 echo/ping 应收到 echo/ping/response"""
        received: list[tuple[str, bytes]] = []
        event = threading.Event()

        sub = _make_client(client_id="test_echo_sub")
        sub.on_message = lambda _c, _u, msg: (received.append((msg.topic, msg.payload)), event.set())
        sub.subscribe("echo/+/response", qos=0)
        time.sleep(0.3)

        pub = _make_client(client_id="test_echo_pub")
        pub.publish("echo/ping", b"hello_echo", qos=0)

        assert event.wait(timeout=5), "未收到 echo 响应"
        assert received[0][0] == "echo/ping/response"
        assert received[0][1] == b"hello_echo"

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()


@pytest.mark.integration
class TestMqttRetain:
    """Retain 保留消息测试"""

    def test_retained_message_delivered_to_new_subscriber(self) -> None:
        """新订阅者应收到保留消息"""
        # 先发布 retain 消息
        pub = _make_client(client_id="test_retain_pub")
        pub.publish("test/retain", b"retained_payload", qos=0, retain=True)
        time.sleep(0.5)
        pub.loop_stop()
        pub.disconnect()

        # 新订阅者应收到保留消息
        received: list[bytes] = []
        event = threading.Event()

        sub = _make_client(client_id="test_retain_sub")
        sub.on_message = lambda _c, _u, msg: (received.append(msg.payload), event.set())
        sub.subscribe("test/retain", qos=0)

        assert event.wait(timeout=5), "未收到保留消息"
        assert received[0] == b"retained_payload"

        # 清理 retain 消息 (发空 payload)
        pub2 = _make_client(client_id="test_retain_clean")
        pub2.publish("test/retain", b"", qos=0, retain=True)
        time.sleep(0.3)
        pub2.loop_stop()
        pub2.disconnect()

        sub.loop_stop()
        sub.disconnect()


@pytest.mark.integration
class TestMqttTopicMatch:
    """Topic 通配符匹配测试"""

    def test_single_level_wildcard(self) -> None:
        """单级通配符 + 匹配"""
        received: list[str] = []
        event = threading.Event()

        sub = _make_client(client_id="test_wild_sub")
        sub.on_message = lambda _c, _u, msg: (received.append(msg.topic), event.set())
        sub.subscribe("sensor/+/temperature", qos=0)
        time.sleep(0.3)

        pub = _make_client(client_id="test_wild_pub")
        pub.publish("sensor/room1/temperature", b"22.5", qos=0)

        assert event.wait(timeout=5), "未收到通配符匹配消息"
        assert received[0] == "sensor/room1/temperature"

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()

    def test_multi_level_wildcard(self) -> None:
        """多级通配符 # 匹配"""
        received: list[str] = []
        event = threading.Event()

        sub = _make_client(client_id="test_hash_sub")
        sub.on_message = lambda _c, _u, msg: (received.append(msg.topic), event.set())
        sub.subscribe("home/#", qos=0)
        time.sleep(0.3)

        pub = _make_client(client_id="test_hash_pub")
        pub.publish("home/floor1/room/temp", b"25.0", qos=0)

        assert event.wait(timeout=5), "未收到多级通配符匹配消息"
        assert received[0] == "home/floor1/room/temp"

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()
