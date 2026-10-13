// protocol_test.cpp —— B2.1/B2.2 测试驱动（纯 C++，无 Qt 依赖）
// 灌入接口规范 §2.3 全部向量: V1/V2/V3 + E1~E4，附 2 个增强用例
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -o protocol_test protocol.cpp protocol_test.cpp
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

using proto::FrameParser;
using proto::ParsedFrame;
using proto::SensorReading;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { ++g_passed; }                                             \
        else { ++g_failed; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
    } while (0)

// --- 接口规范 §2.3 标准向量 ---
static const std::vector<uint8_t> V1 = {
    0xAA, 0x55, 0x01, 0x0D, 0x04, 0x01, 0x01, 0x00, 0x02, 0x02,
    0x26, 0x03, 0x01, 0x38, 0x04, 0x01, 0x64, 0xE3};              // 18B
static const std::vector<uint8_t> V2 = {
    0xAA, 0x55, 0x01, 0x07, 0x02, 0x01, 0x00, 0xC8, 0x02, 0x01,
    0xC2, 0x98};                                                  // 12B
static const std::vector<uint8_t> V3 = {
    0xAA, 0x55, 0x01, 0x07, 0x02, 0x01, 0xFF, 0xCC, 0x02, 0x02,
    0x58, 0x32};                                                  // 12B

static void expectSensor(const ParsedFrame& f, int idx, int id, int raw, double v) {
    char msg[128];
    if (idx >= static_cast<int>(f.sensors.size())) {
        snprintf(msg, sizeof msg, "frame sensor[%d] missing", idx);
        CHECK(false, msg);
        return;
    }
    const SensorReading& r = f.sensors[idx];
    snprintf(msg, sizeof msg, "sensor[%d] id=%d", idx, id);
    CHECK(r.id == id, msg);
    snprintf(msg, sizeof msg, "sensor[%d] raw=%d", idx, raw);
    CHECK(r.raw == raw, msg);
    snprintf(msg, sizeof msg, "sensor[%d] value=%.1f", idx, v);
    const double diff = r.value > v ? r.value - v : v - r.value;
    CHECK(diff < 1e-9, msg);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲：崩溃前也能看到已打印内容
    ParsedFrame f;

    // ---------- B2.1 V1: 4个传感器 25.6/55.0/312/356 ----------
    printf("T1  V1 (full 4-sensor frame)\n");
    {
        FrameParser p;
        p.feed(V1);
        CHECK(p.popFrame(f), "V1 parsed");
        CHECK(f.type == 0x01, "V1 type=0x01");
        CHECK(f.sensors.size() == 4, "V1 has 4 sensors");
        expectSensor(f, 0, 1, 256, 25.6);
        expectSensor(f, 1, 2, 550, 55.0);
        expectSensor(f, 2, 3, 312, 312.0);
        expectSensor(f, 3, 4, 356, 356.0);
        CHECK(!p.popFrame(f), "V1 no extra frames");
    }

    // ---------- B2.1 V2: 20.0/45.0 ----------
    printf("T2  V2 (temp+hum)\n");
    {
        FrameParser p;
        p.feed(V2);
        CHECK(p.popFrame(f), "V2 parsed");
        CHECK(f.sensors.size() == 2, "V2 has 2 sensors");
        expectSensor(f, 0, 1, 200, 20.0);
        expectSensor(f, 1, 2, 450, 45.0);
    }

    // ---------- B2.1 V3: -5.2/60.0 (int16 有符号) ----------
    printf("T3  V3 (negative temp)\n");
    {
        FrameParser p;
        p.feed(V3);
        CHECK(p.popFrame(f), "V3 parsed");
        CHECK(f.sensors.size() == 2, "V3 has 2 sensors");
        expectSensor(f, 0, 1, -52, -5.2);
        expectSensor(f, 1, 2, 600, 60.0);
    }

    // ---------- B2.2 E1: 错校验 → 丢弃整帧，后续帧正常 ----------
    printf("T4  E1 (bad checksum, next frame still OK)\n");
    {
        FrameParser p;
        std::vector<uint8_t> e1 = V1;
        e1.back() = 0xE2;                      // E3 → E2
        p.feed(e1);
        CHECK(!p.popFrame(f), "E1 frame dropped");
        CHECK(p.droppedFrames() == 1, "E1 dropped count = 1");
        p.feed(V2);                            // 后续帧必须正常
        CHECK(p.popFrame(f) && f.sensors.size() == 2, "frame after E1 parses OK");
        CHECK(p.droppedFrames() == 1, "still 1 drop after V2");
    }

    // ---------- B2.2 E2: 粘包 V1+V2 → 两帧都解出 ----------
    printf("T5  E2 (coalesced V1+V2)\n");
    {
        FrameParser p;
        std::vector<uint8_t> both = V1;
        both.insert(both.end(), V2.begin(), V2.end());
        p.feed(both);
        CHECK(p.popFrame(f) && f.sensors.size() == 4, "E2 frame1 (V1) parsed");
        const bool f1v1 = f.sensors.size() == 4 &&
                          f.sensors[0].raw == 256 && f.sensors[3].raw == 356;
        CHECK(f1v1, "E2 frame1 values = V1");
        CHECK(p.popFrame(f) && f.sensors.size() == 2, "E2 frame2 (V2) parsed");
        const bool f2v2 = f.sensors.size() == 2 &&
                          f.sensors[0].raw == 200 && f.sensors[1].raw == 450;
        CHECK(f2v2, "E2 frame2 values = V2");
        CHECK(!p.popFrame(f), "E2 no extra frames");
    }

    // ---------- B2.2 E3: 半帧缓冲，补齐后解出 ----------
    printf("T6  E3 (half frame then completion)\n");
    {
        FrameParser p;
        p.feed(V1.data(), 9);                  // 前 9 字节
        CHECK(!p.popFrame(f), "E3 half frame not emitted early");
        CHECK(p.bufferedBytes() == 9, "E3 9 bytes buffered");
        p.feed(V1.data() + 9, V1.size() - 9);  // 补齐余下
        CHECK(p.popFrame(f) && f.sensors.size() == 4, "E3 frame completes");
        CHECK(f.sensors[0].raw == 256, "E3 decoded value correct");
    }

    // ---------- B2.2 E4: 噪声前缀 FF 12 00 + V1 → 跳过后解出 ----------
    printf("T7  E4 (noise prefix before frame)\n");
    {
        FrameParser p;
        const uint8_t noise[3] = {0xFF, 0x12, 0x00};
        p.feed(noise, 3);
        p.feed(V1);
        CHECK(p.popFrame(f) && f.sensors.size() == 4, "E4 V1 parsed after noise");
        CHECK(f.sensors[1].raw == 550, "E4 values correct");
        CHECK(p.droppedFrames() == 0, "E4 no drop counted (noise skipped)");
    }

    // ---------- 增强: 逐字节灌入 V1（极限分包） ----------
    printf("T8  byte-by-byte feed of V1\n");
    {
        FrameParser p;
        for (uint8_t b : V1) p.feed(&b, 1);
        CHECK(p.popFrame(f) && f.sensors.size() == 4, "byte-by-byte V1 parsed");
    }

    // ---------- 增强: 未知类型帧 (预留0x02) → 丢弃不崩溃 ----------
    printf("T9  reserved type 0x02 frame dropped\n");
    {
        FrameParser p;
        const uint8_t hb[6] = {0xAA, 0x55, 0x02, 0x01, 0x05, 0x08};
        p.feed(hb, 6);
        CHECK(!p.popFrame(f), "reserved-type frame not emitted");
        CHECK(p.droppedFrames() == 1, "reserved-type counted as drop");
        p.feed(V1);
        CHECK(p.popFrame(f) && f.sensors.size() == 4, "frame after reserved-type OK");
    }

    printf("\n===== RESULT: %d passed, %d failed =====\n", g_passed, g_failed);
    if (g_failed == 0) printf("B2.1/B2.2 全部 7 个标准向量 + 2 个增强用例通过\n");
    return g_failed == 0 ? 0 : 1;
}
