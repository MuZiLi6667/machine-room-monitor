// serialclient_main.cpp —— B2.3/B2.4 联试客户端（控制台版）
// QSerialPort 115200-8-N-1 打开串口 → 字节流灌入 FrameParser → 打印解析结果
// 用法: serialclient.exe <COM口> <运行秒数>   默认 COM2 10 秒
#include <QCoreApplication>
#include <QSerialPort>
#include <QElapsedTimer>
#include <cstdio>
#include <cstdlib>

#include "protocol.h"

static void printFrame(double ms, const proto::ParsedFrame& f) {
    printf("[%6.0fms] FRAME type=%02X n=%d :", ms, f.type, (int)f.sensors.size());
    for (const auto& s : f.sensors) {
        if (s.id == 1 || s.id == 2)
            printf(" id%d=%.1f", s.id, s.value);
        else
            printf(" id%d=%d", s.id, s.raw);
    }
    printf("\n");
    fflush(stdout);
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);

    const char* portName = (argc > 1) ? argv[1] : "COM2";
    const int runSeconds = (argc > 2) ? atoi(argv[2]) : 10;

    QSerialPort port;
    port.setPortName(portName);
    port.setBaudRate(QSerialPort::Baud115200);  // 接口规范§1: 115200-8-N-1
    port.setDataBits(QSerialPort::Data8);
    port.setParity(QSerialPort::NoParity);
    port.setStopBits(QSerialPort::OneStop);
    port.setFlowControl(QSerialPort::NoFlowControl);

    if (!port.open(QIODevice::ReadOnly)) {
        printf("OPEN_FAIL %s : %s\n", portName, port.errorString().toLocal8Bit().constData());
        return 1;
    }
    printf("OPEN_OK %s @115200-8-N-1 (QSerialPort %s), run %ds\n",
           portName, qVersion(), runSeconds);

    proto::FrameParser parser;
    QElapsedTimer timer;
    timer.start();
    long lastDropped = 0;
    long totalBytes = 0;

    while (timer.elapsed() < runSeconds * 1000) {
        if (port.waitForReadyRead(100)) {
            const QByteArray d = port.readAll();
            totalBytes += d.size();
            parser.feed(reinterpret_cast<const uint8_t*>(d.constData()),
                        static_cast<size_t>(d.size()));
            proto::ParsedFrame f;
            while (parser.popFrame(f))
                printFrame((double)timer.elapsed(), f);
            if ((long)parser.droppedFrames() != lastDropped) {
                lastDropped = (long)parser.droppedFrames();
                printf("[%6.0fms] DROP detected (total dropped=%ld)\n",
                       (double)timer.elapsed(), lastDropped);
            }
        }
    }
    port.close();

    printf("SUMMARY bytes=%ld valid_frames=%ld dropped_frames=%ld buffered=%zu\n",
           totalBytes, (long)parser.validFrames(), (long)parser.droppedFrames(),
           parser.bufferedBytes());
    return 0;
}
