// WS/HTTP 帧解析模糊测试 (libFuzzer)
// 用法: cmake -DTHUNDER_BUILD_FUZZ=ON && ./fuzz_ws_frame corpus/
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    
    // WS frame 解析: opcode + mask + payload length
    uint8_t opcode = data[0] & 0x0F;
    bool masked = (data[1] & 0x80) != 0;
    size_t payload_len = data[1] & 0x7F;
    size_t offset = 2;
    
    if (payload_len == 126) {
        if (offset + 2 > size) return 0;
        payload_len = (data[offset] << 8) | data[offset+1];
        offset += 2;
    } else if (payload_len == 127) {
        if (offset + 8 > size) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | data[offset+i];
        offset += 8;
    }
    
    // mask key
    if (masked) {
        if (offset + 4 > size) return 0;
        offset += 4;
    }
    
    // payload
    if (offset + payload_len > size) return 0;
    
    // 验证: 遍历 payload 不崩溃
    volatile uint8_t sum = 0;
    for (size_t i = offset; i < offset + payload_len && i < size; i++)
        sum ^= data[i];
    
    (void)opcode;  // all opcodes are valid input
    return 0;
}
