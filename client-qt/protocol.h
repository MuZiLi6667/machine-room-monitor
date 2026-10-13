// protocol.h —— 串口帧解析器（接口规范 §2，纯 C++，不依赖 Qt，便于单测）
// 帧格式: 帧头0xAA55(2B) | 帧类型(1B) | 数据长度N(1B) | 数据域(N B) | SUM(1B)
// 数据域: n(1B) + n × (传感器编号(1B) + 数值(2B 大端有符号int16))
// SUM: 帧类型字节到数据域最后一字节逐字节累加取低8位
// 粘包/错帧: 搜帧头 → 校验长度(≤64) → 校验SUM；失败则丢弃该帧头重新同步
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace proto {

struct SensorReading {
    int id = 0;        // 1=温度 2=湿度 3=烟雾 4=光照
    int raw = 0;       // 原始 int16（温/湿为真实值×10，烟雾/光照为整数原值）
    double value = 0;  // 解码值: id 1/2 → raw/10.0；id 3/4 → raw
};

struct ParsedFrame {
    uint8_t type = 0;
    std::vector<SensorReading> sensors;
};

class FrameParser {
public:
    // 灌入任意长度字节流（可分次到达、可粘包、可含噪声）
    void feed(const uint8_t* data, std::size_t len);
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

    // 按序取出解析成功的帧
    bool popFrame(ParsedFrame& out);

    // 统计
    std::size_t validFrames() const { return valid_; }
    std::size_t droppedFrames() const { return dropped_; }
    std::size_t bufferedBytes() const { return buf_.size(); }

    static constexpr uint8_t HEADER0 = 0xAA;
    static constexpr uint8_t HEADER1 = 0x55;
    static constexpr uint8_t TYPE_SENSOR = 0x01;   // 本期仅用；0x02心跳/0x03应答预留
    static constexpr std::size_t MAX_PAYLOAD = 64; // 数据长度 ≤ 64 合法

private:
    void parse();

    std::vector<uint8_t> buf_;      // 内部缓冲（半帧等待）
    std::deque<ParsedFrame> frames_;
    std::size_t valid_ = 0;
    std::size_t dropped_ = 0;
};

} // namespace proto

#endif // PROTOCOL_H
