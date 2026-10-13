// main_gui.cpp —— GUI 入口
// 用法: client.exe                     正常手动模式
//       client.exe --auto COM2 15 <结果文件> [TCP主机 TCP端口]  自动验收模式
#include <QApplication>

#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    if (argc >= 4 && QString(argv[1]) == "--auto") {
        const QString port = QString::fromLocal8Bit(argv[2]);
        const int seconds = atoi(argv[3]);
        const QString resultFile =
            (argc >= 5) ? QString::fromLocal8Bit(argv[4])
                        : QStringLiteral("auto_result.txt");
        // 可选第5/6参数: TCP 服务器地址端口 (B4 验收用)
        const QString tcpHost = (argc >= 7) ? QString::fromLocal8Bit(argv[5]) : QString();
        const quint16 tcpPort = (argc >= 7) ? (quint16)atoi(argv[6]) : 0;
        w.startAutoMode(port, seconds, resultFile, tcpHost, tcpPort);
    }
    return app.exec();
}
