// protocol.cpp —— 帧解析器实现（见 protocol.h 注释与接口规范 §2）
#include "protocol.h"

namespace proto {
namespace {
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
}

void FrameParser::feed(const uint8_t* data, std::size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    parse();
}

bool FrameParser::popFrame(ParsedFrame& out) {
    if (frames_.empty()) return false;
    out = std::move(frames_.front());
    frames_.pop_front();
    return true;
}

void FrameParser::parse() {
    std::size_t pos = 0;
    while (true) {
        // 1) 搜索帧头 0xAA 0x55
        std::size_t hdr = kNpos;
        for (std::size_t i = pos; i + 1 < buf_.size(); ++i) {
            if (buf_[i] == HEADER0 && buf_[i + 1] == HEADER1) { hdr = i; break; }
        }
        if (hdr == kNpos) {
            // 无完整帧头：若末尾恰是 0xAA，保留它等待下一个 0x55，其余丢弃
            std::size_t keep = buf_.size();
            if (!buf_.empty() && buf_.back() == HEADER0) keep = buf_.size() - 1;
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(keep));
            return;
        }

        // 2) 帧头后至少需要 类型+长度 两字节
        if (hdr + 4 > buf_.size()) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(hdr));
            return; // 半帧，缓冲等待
        }

        const uint8_t type = buf_[hdr + 2];
        const uint8_t n = buf_[hdr + 3];
        const std::size_t total = 5 + static_cast<std::size_t>(n); // 2+1+1+N+1

        // 3) 长度校验：n ≥ 1 且 ≤ 64（不合法 → 丢帧重同步）
        if (type != TYPE_SENSOR || n < 1 || n > MAX_PAYLOAD) {
            ++dropped_;
            pos = hdr + 1; // 丢弃当前帧头，从下一字节继续搜索
            continue;
        }

        // 4) 整帧未到齐 → 缓冲等待（E3 半帧场景）
        if (hdr + total > buf_.size()) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(hdr));
            return;
        }

        // 5) SUM 校验：帧类型..数据域最后字节 累加取低 8 位
        uint8_t sum = 0;
        for (std::size_t k = hdr + 2; k <= hdr + 3 + static_cast<std::size_t>(n); ++k)
            sum = static_cast<uint8_t>(sum + buf_[k]);
        if (sum != buf_[hdr + 4 + static_cast<std::size_t>(n)]) {
            ++dropped_;
            pos = hdr + 1;
            continue;
        }

        // 6) 数据域结构校验：n == 1 + 3*cnt（规范§2.2: N = 1 + 3×n）
        const std::size_t d0 = hdr + 4;
        const uint8_t cnt = buf_[d0];
        if (cnt < 1 || 1 + 3u * cnt != static_cast<std::size_t>(n)) {
            ++dropped_;
            pos = hdr + 1;
            continue;
        }

        // 7) 解码 n 个传感器（大端 int16）
        ParsedFrame f;
        f.type = type;
        f.sensors.reserve(cnt);
        for (int k = 0; k < cnt; ++k) {
            SensorReading r;
            r.id = buf_[d0 + 1 + 3u * k];
            const uint16_t be = static_cast<uint16_t>(
                (static_cast<uint16_t>(buf_[d0 + 2 + 3u * k]) << 8) |
                static_cast<uint16_t>(buf_[d0 + 3 + 3u * k]));
            r.raw = static_cast<int16_t>(be);
            // 规范§1精度: 温度/湿度 ×10 存储(1位小数); 烟雾/光照直接存整数
            r.value = (r.id == 1 || r.id == 2) ? r.raw / 10.0 : static_cast<double>(r.raw);
            f.sensors.push_back(r);
        }
        frames_.push_back(std::move(f));
        ++valid_;
        pos = hdr + total;
    }
}

} // namespace proto
