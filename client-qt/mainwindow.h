// mainwindow.h —— B3 客户端主窗口（机房环境多指标远程监测）
// 六大区域: 串口配置 / 服务器连接 / 实时数据 / 曲线(QCustomPlot×4) / 消息 / 日志
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QJsonObject>
#include <QMainWindow>
#include <QSerialPort>
#include <QElapsedTimer>
#include <array>

#include "protocol.h"

class QComboBox;
class QPushButton;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QCheckBox;
class QTableWidget;
class QListWidget;
class QCustomPlot;
class QTimer;
class TcpClient;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 自动化验收模式: 自动开串口(可选TCP), N 秒后写结果文件并退出
    void startAutoMode(const QString& port, int seconds, const QString& resultFile,
                       const QString& tcpHost = QString(), quint16 tcpPort = 0);

private slots:
    void refreshPorts();
    void toggleSerial();
    void onReadyRead();
    void onChartTick();
    void onExportLog();
    void onConnectToggle();   // B4: QTcpSocket 连接 → LOGIN → LOGIN_ACK → 状态指示
    void onJsonMsg(const QJsonObject& msg);  // B4: ANALYSIS/ALARM/ERROR 驱动消息区

private:
    void buildUi();
    QCustomPlot* makePlot(const QString& title, const QColor& color);
    void log(const QString& tag, const QString& text);
    void handleFrame(const proto::ParsedFrame& f, double nowSec);
    void updateValueLabels();
    void resetCurves();
    void finishAuto();

    // --- 串口配置区 ---
    QComboBox* portBox_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QLabel* serialParamLabel_ = nullptr;

    // --- 连接配置区 ---
    QLineEdit* ipEdit_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QLineEdit* idEdit_ = nullptr;
    QPushButton* connBtn_ = nullptr;
    QLabel* connState_ = nullptr;

    // --- 实时数据区 ---
    std::array<QLabel*, 4> valueLabels_ = {nullptr, nullptr, nullptr, nullptr};

    // --- 曲线区 ---
    std::array<QCustomPlot*, 4> plots_ = {nullptr, nullptr, nullptr, nullptr};

    // --- 消息区 ---
    QTableWidget* analysisTable_ = nullptr;
    QListWidget* alarmList_ = nullptr;

    // --- 日志区 ---
    QPlainTextEdit* logView_ = nullptr;
    QCheckBox* hexChk_ = nullptr;
    QCheckBox* parseChk_ = nullptr;
    QCheckBox* netChk_ = nullptr;
    QPushButton* exportBtn_ = nullptr;

    // --- 状态 ---
    QSerialPort port_;
    proto::FrameParser parser_;
    QTimer* chartTimer_ = nullptr;
    QElapsedTimer clock_;
    std::array<bool, 4> hasData_ = {false, false, false, false};
    std::array<double, 4> latest_ = {0, 0, 0, 0};
    qint64 lastDataMs_ = -1;
    long totalBytes_ = 0;
    long uiTicks_ = 0;
    TcpClient* tcp_ = nullptr;

    // HIGH 级告警弹窗节流：同一传感器 60 秒内只弹一次（防持续告警时
    // 模态弹窗风暴遮挡主窗口导致界面无法操作/关闭）；下标 0 备用
    std::array<qint64, 5> lastHighPopupMs_ = {-60000, -60000, -60000, -60000, -60000};

    // 自动验收模式
    bool autoMode_ = false;
    int autoSeconds_ = 0;
    QString autoResultFile_;
    QString autoTcpHost_;
    quint16 autoTcpPort_ = 0;
};

#endif // MAINWINDOW_H
