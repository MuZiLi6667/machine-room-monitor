// mainwindow.cpp —— B3 客户端主窗口实现
// 说明: 串口用 readyRead 异步收包(不阻塞UI), 解析为轻量纯C++, 曲线 500ms 定时刷新,
//       10 分钟滚动窗口 300 点/曲线, UI 线程无耗时操作, 满足验收"无卡顿"。
#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "qcustomplot.h"
#include "tcpclient.h"

#include <QJsonArray>

namespace {
const char* kSensorNames[4] = {"温度 (℃)", "湿度 (%RH)", "烟雾 (ADC)", "光照 (lux)"};
const QColor kSensorColors[4] = {QColor(0xE7, 0x4C, 0x3C),   // 红
                                 QColor(0x34, 0x98, 0xDB),   // 蓝
                                 QColor(0xE6, 0x7E, 0x22),   // 橙
                                 QColor(0x27, 0xAE, 0x60)};  // 绿
constexpr int kWindowSecs = 600;   // 10 分钟滚动窗口
constexpr qint64 kOfflineMs = 6000; // >6s 无数据视为离线
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("机房环境多指标监测客户端");
    clock_.start();
    buildUi();

    chartTimer_ = new QTimer(this);
    connect(chartTimer_, &QTimer::timeout, this, &MainWindow::onChartTick);
    chartTimer_->start(500);

    connect(&port_, &QSerialPort::readyRead, this, &MainWindow::onReadyRead);
    refreshPorts();

    // --- B4: TCP 模块接入 ---
    tcp_ = new TcpClient(this);
    connect(tcp_, &TcpClient::stateChanged, this, [this](const QString& text, const QString& color) {
        connState_->setText(text);
        connState_->setStyleSheet(QString("color:%1;").arg(color));
        if (text.contains("未连接"))
            connBtn_->setText("连接服务器");
    });
    connect(tcp_, &TcpClient::logEvent, this, [this](const QString& text) {
        log("网络", text);
    });
    connect(tcp_, &TcpClient::jsonReceived, this, &MainWindow::onJsonMsg);

    if (!autoMode_) {
        log("系统", QString("客户端启动, Qt %1, 就绪").arg(QT_VERSION_STR));
    }
}

void MainWindow::buildUi() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QHBoxLayout(central);

    // ================= 左列: 数据 + 曲线 =================
    auto* left = new QVBoxLayout();
    root->addLayout(left, 3);

    // --- 串口配置区 ---
    auto* serialBox = new QGroupBox("① 串口配置", central);
    auto* sh = new QHBoxLayout(serialBox);
    portBox_ = new QComboBox(serialBox);
    portBox_->setMinimumWidth(110);
    refreshBtn_ = new QPushButton("刷新", serialBox);
    openBtn_ = new QPushButton("打开串口", serialBox);
    serialParamLabel_ = new QLabel("115200-8-N-1 (固定)", serialBox);
    sh->addWidget(new QLabel("端口:", serialBox));
    sh->addWidget(portBox_);
    sh->addWidget(refreshBtn_);
    sh->addWidget(openBtn_);
    sh->addWidget(serialParamLabel_);
    sh->addStretch(1);
    connect(refreshBtn_, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::toggleSerial);

    // --- 连接配置区 ---
    auto* connBox = new QGroupBox("② 服务器连接", central);
    auto* ch = new QHBoxLayout(connBox);
    ipEdit_ = new QLineEdit("127.0.0.1", connBox);
    ipEdit_->setMaximumWidth(120);
    portEdit_ = new QLineEdit("9000", connBox);
    portEdit_->setMaximumWidth(70);
    idEdit_ = new QLineEdit(QHostInfo::localHostName(), connBox);
    idEdit_->setMaximumWidth(140);
    connBtn_ = new QPushButton("连接服务器", connBox);
    connState_ = new QLabel("● 未连接", connBox);
    connState_->setStyleSheet("color:#888;");
    ch->addWidget(new QLabel("IP:", connBox));
    ch->addWidget(ipEdit_);
    ch->addWidget(new QLabel("端口:", connBox));
    ch->addWidget(portEdit_);
    ch->addWidget(new QLabel("client_id:", connBox));
    ch->addWidget(idEdit_);
    ch->addWidget(connBtn_);
    ch->addWidget(connState_);
    ch->addStretch(1);
    connect(connBtn_, &QPushButton::clicked, this, &MainWindow::onConnectToggle);

    auto* cfgRow = new QHBoxLayout();
    cfgRow->addWidget(serialBox, 1);
    cfgRow->addWidget(connBox, 1);
    left->addLayout(cfgRow);

    // --- 实时数据区 ---
    auto* valueBox = new QGroupBox("③ 实时数据", central);
    auto* vg = new QGridLayout(valueBox);
    QFont big;
    big.setPointSize(22);
    big.setBold(true);
    for (int i = 0; i < 4; ++i) {
        auto* cell = new QVBoxLayout();
        auto* name = new QLabel(kSensorNames[i], valueBox);
        name->setAlignment(Qt::AlignCenter);
        valueLabels_[i] = new QLabel("—", valueBox);
        valueLabels_[i]->setFont(big);
        valueLabels_[i]->setAlignment(Qt::AlignCenter);
        valueLabels_[i]->setStyleSheet("color:#AAA;");   // 无数据 = 离线灰
        cell->addWidget(name);
        cell->addWidget(valueLabels_[i]);
        vg->addLayout(cell, i / 2, i % 2);
    }
    left->addWidget(valueBox);

    // --- 曲线区: 4 子图 2×2 ---
    auto* plotBox = new QGroupBox("④ 实时曲线 (最近10分钟滚动)", central);
    auto* pg = new QGridLayout(plotBox);
    for (int i = 0; i < 4; ++i) {
        plots_[i] = makePlot(kSensorNames[i], kSensorColors[i]);
        pg->addWidget(plots_[i], i / 2, i % 2);
    }
    left->addWidget(plotBox, 1);

    // ================= 右列: 消息 + 日志 =================
    auto* right = new QVBoxLayout();
    root->addLayout(right, 2);

    // --- 消息区 ---
    auto* msgBox = new QGroupBox("⑤ 消息区 (ANALYSIS / ALARM, 数据由B4接入)", central);
    auto* mv = new QVBoxLayout(msgBox);
    analysisTable_ = new QTableWidget(0, 6, msgBox);
    analysisTable_->setHorizontalHeaderLabels({"传感器", "平均", "最大", "最小", "标准差", "超标"});
    analysisTable_->horizontalHeader()->setStretchLastSection(true);
    analysisTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mv->addWidget(new QLabel("ANALYSIS 统计:", msgBox));
    mv->addWidget(analysisTable_);
    mv->addWidget(new QLabel("ALARM 告警列表:", msgBox));
    alarmList_ = new QListWidget(msgBox);
    mv->addWidget(alarmList_);
    right->addWidget(msgBox, 1);

    // --- 日志区 ---
    auto* logBox = new QGroupBox("⑥ 日志", central);
    auto* lv = new QVBoxLayout(logBox);
    auto* lrow = new QHBoxLayout();
    hexChk_ = new QCheckBox("原始HEX", logBox);
    parseChk_ = new QCheckBox("解析结果", logBox);
    parseChk_->setChecked(true);
    netChk_ = new QCheckBox("网络收发", logBox);
    exportBtn_ = new QPushButton("导出日志", logBox);
    lrow->addWidget(hexChk_);
    lrow->addWidget(parseChk_);
    lrow->addWidget(netChk_);
    lrow->addStretch(1);
    lrow->addWidget(exportBtn_);
    lv->addLayout(lrow);
    logView_ = new QPlainTextEdit(logBox);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);
    lv->addWidget(logView_);
    right->addWidget(logBox, 1);
    connect(exportBtn_, &QPushButton::clicked, this, &MainWindow::onExportLog);

    resize(1280, 860);
}

QCustomPlot* MainWindow::makePlot(const QString& title, const QColor& color) {
    auto* plot = new QCustomPlot(this);
    plot->addGraph();
    plot->graph(0)->setPen(QPen(color, 1.6));
    plot->graph(0)->setName(title);
    auto* te = new QCPTextElement(plot, title, QFont(font().family(), 10, QFont::Bold));
    plot->plotLayout()->addElement(0, 0, te);
    QSharedPointer<QCPAxisTickerDateTime> ticker(new QCPAxisTickerDateTime());
    ticker->setDateTimeFormat("HH:mm:ss");
    ticker->setDateTimeSpec(Qt::LocalTime);          // 关键: 横轴用本地时间
    plot->xAxis->setTicker(ticker);
    plot->xAxis->setLabel("");
    plot->yAxis->setLabel("");
    plot->setBackground(QColor(0xFA, 0xFA, 0xFA));
    plot->replot();
    return plot;
}

void MainWindow::refreshPorts() {
    portBox_->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto& p : ports)
        portBox_->addItem(p.portName());
    if (portBox_->count() == 0)
        portBox_->addItem("COM1");
    log("系统", QString("枚举到 %1 个串口").arg(ports.size()));
}

void MainWindow::toggleSerial() {
    if (port_.isOpen()) {
        port_.close();
        resetCurves();
        openBtn_->setText("打开串口");
        log("串口", "已关闭");
        return;
    }
    port_.setPortName(portBox_->currentText());
    port_.setBaudRate(QSerialPort::Baud115200);   // 接口规范§1: 115200-8-N-1
    port_.setDataBits(QSerialPort::Data8);
    port_.setParity(QSerialPort::NoParity);
    port_.setStopBits(QSerialPort::OneStop);
    port_.setFlowControl(QSerialPort::NoFlowControl);
    if (!port_.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "串口错误",
                              QString("打开 %1 失败: %2")
                                  .arg(port_.portName(), port_.errorString()));
        log("串口", QString("打开失败: %1").arg(port_.errorString()));
        return;
    }
    openBtn_->setText("关闭串口");
    resetCurves();
    lastDataMs_ = clock_.elapsed();
    log("串口", QString("已打开 %1 @115200-8-N-1").arg(port_.portName()));
}

// 清空曲线数据并复位 Y 轴/数值标签, 保证每次打开串口从干净状态开始
void MainWindow::resetCurves() {
    for (int i = 0; i < 4; ++i) {
        plots_[i]->graph(0)->data()->clear();
        plots_[i]->yAxis->setRange(0, 1);
        plots_[i]->replot(QCustomPlot::rpQueuedReplot);
    }
    hasData_ = {false, false, false, false};
    latest_ = {0, 0, 0, 0};
    updateValueLabels();
}

void MainWindow::onReadyRead() {
    const QByteArray d = port_.readAll();
    if (d.isEmpty()) return;
    totalBytes_ += d.size();
    if (hexChk_->isChecked()) {
        QString hex;
        hex.reserve(d.size() * 3);
        for (unsigned char b : d)
            hex += QString("%1 ").arg(b, 2, 16, QChar('0'));
        log("HEX", hex.trimmed());
    }
    parser_.feed(reinterpret_cast<const uint8_t*>(d.constData()),
                 static_cast<size_t>(d.size()));
    proto::ParsedFrame f;
    const double nowSec = (double)QDateTime::currentSecsSinceEpoch();
    while (parser_.popFrame(f))
        handleFrame(f, nowSec);
}

void MainWindow::handleFrame(const proto::ParsedFrame& f, double nowSec) {
    QJsonArray dataSensors;   // B4.2: DATA 转发
    for (const auto& s : f.sensors) {
        if (s.id < 1 || s.id > 4) continue;
        latest_[s.id - 1] = s.value;
        hasData_[s.id - 1] = true;
        plots_[s.id - 1]->graph(0)->addData(nowSec, s.value);
        QJsonObject o;
        o.insert("id", s.id);
        o.insert("v", (s.id == 1 || s.id == 2) ? qRound(s.value * 10) / 10.0
                                               : (double)s.raw);
        dataSensors.append(o);
    }
    if (!dataSensors.isEmpty() && tcp_ && tcp_->isLoggedIn())
        tcp_->sendData(dataSensors);
    lastDataMs_ = clock_.elapsed();
    if (parseChk_->isChecked()) {
        QString line;
        for (const auto& s : f.sensors) {
            line += (s.id == 1 || s.id == 2)
                        ? QString("id%1=%2 ").arg(s.id).arg(s.value, 0, 'f', 1)
                        : QString("id%1=%2 ").arg(s.id).arg(s.raw);
        }
        log("解析", line.trimmed());
    }
    updateValueLabels();
}

void MainWindow::onChartTick() {
    ++uiTicks_;
    const qint64 nowMs = clock_.elapsed();
    const double now = (double)QDateTime::currentSecsSinceEpoch();

    for (int i = 0; i < 4; ++i) {
        auto* g = plots_[i]->graph(0);
        g->data()->removeBefore(now - kWindowSecs);
        plots_[i]->xAxis->setRange(now - kWindowSecs, now);
        if (g->data()->size() > 0) {
            // QCP 2.1.1 的 rescaleValueAxis 第一参数是 onlyEnlarge(只扩不缩),
            // 不能用; 手动取"可见窗口内"数据值范围, 加 15% 余量, 只扩不缩防抖动
            bool found = false;
            const QCPRange vr = g->data()->valueRange(
                found, QCP::sdBoth, QCPRange(now - kWindowSecs, now));
            if (found) {
                double span = vr.size();
                if (span < 1e-9) span = 2.0;
                const double center = (vr.lower + vr.upper) * 0.5;
                QCPRange target(center - span * 0.65, center + span * 0.65);
                target.expand(plots_[i]->yAxis->range());
                plots_[i]->yAxis->setRange(target);
            }
        }
        plots_[i]->replot(QCustomPlot::rpQueuedReplot);
    }
    // 离线检测: >6s 无新帧 → 值标签变灰
    const bool offline = (lastDataMs_ < 0) || (nowMs - lastDataMs_ > kOfflineMs);
    if (offline)
        updateValueLabels();
}

void MainWindow::updateValueLabels() {
    const qint64 nowMs = clock_.elapsed();
    const bool offline = (lastDataMs_ < 0) || (nowMs - lastDataMs_ > kOfflineMs);
    for (int i = 0; i < 4; ++i) {
        if (offline || !hasData_[i]) {
            valueLabels_[i]->setText("—");
            valueLabels_[i]->setStyleSheet("color:#AAA;");          // 离线灰
        } else {
            const QString txt = (i == 0 || i == 1)
                                    ? QString::number(latest_[i], 'f', 1)
                                    : QString::number(latest_[i], 'f', 0);
            valueLabels_[i]->setText(txt);
            valueLabels_[i]->setStyleSheet(
                QString("color:%1;").arg(kSensorColors[i].name())); // 正常: 传感器色
        }
    }
}

void MainWindow::log(const QString& tag, const QString& text) {
    if (tag == "HEX" && !hexChk_->isChecked()) return;
    if (tag == "解析" && !parseChk_->isChecked()) return;
    if (tag == "网络" && !netChk_->isChecked()) return;
    logView_->appendPlainText(QString("%1 [%2] %3")
                                  .arg(QDateTime::currentDateTime()
                                           .toString("HH:mm:ss.zzz"), tag, text));
}

void MainWindow::onExportLog() {
    const QString file = QFileDialog::getSaveFileName(this, "导出日志",
                                                      "client-log.txt",
                                                      "Text (*.txt);;All (*)");
    if (file.isEmpty()) return;
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(logView_->toPlainText().toUtf8());
        log("系统", QString("日志已导出: %1").arg(file));
    } else {
        QMessageBox::warning(this, "导出失败", f.errorString());
    }
}

void MainWindow::onConnectToggle() {
    if (tcp_->busy()) {                    // 已在连接/登录/重连中 → 断开
        tcp_->stop();
        connBtn_->setText("连接服务器");
        return;
    }
    const QString ip = ipEdit_->text().trimmed();
    bool ok = false;
    const quint16 p = portEdit_->text().toUShort(&ok);
    if (ip.isEmpty() || !ok || p == 0) {
        QMessageBox::warning(this, "参数错误", "请填写有效的服务器 IP 和端口");
        return;
    }
    const QString id = idEdit_->text().trimmed().isEmpty()
                           ? QStringLiteral("PC-01") : idEdit_->text().trimmed();
    tcp_->connectTo(ip, p, id);
    connBtn_->setText("断开");
}

void MainWindow::onJsonMsg(const QJsonObject& msg) {
    const QString type = msg.value("type").toString();

    if (type == "ANALYSIS") {
        const QJsonArray sensors = msg.value("sensors").toArray();
        analysisTable_->setRowCount(sensors.size());
        int row = 0;
        for (const auto& sv : sensors) {
            const QJsonObject s = sv.toObject();
            const int id = s.value("id").toInt();
            const QString name = (id >= 1 && id <= 4)
                                     ? QString::fromUtf8(kSensorNames[id - 1])
                                     : QString("传感器%1").arg(id);
            auto cell = [this, row](int col, const QString& text) {
                analysisTable_->setItem(row, col, new QTableWidgetItem(text));
            };
            const double avg = s.value("avg").toDouble();
            const double mx = s.value("max").toDouble();
            const double mn = s.value("min").toDouble();
            const double std_ = s.value("std").toDouble();
            cell(0, name);
            cell(1, QString::number(avg, 'f', (id == 1 || id == 2) ? 1 : 0));
            cell(2, QString::number(mx, 'f', (id == 1 || id == 2) ? 1 : 0));
            cell(3, QString::number(mn, 'f', (id == 1 || id == 2) ? 1 : 0));
            cell(4, QString::number(std_, 'f', 2));
            cell(5, QString::number(s.value("over_count").toInt()));
            if (s.value("over_count").toInt() > 0) {   // 有超标: 该行标红
                for (int c = 0; c < 6; ++c) {
                    if (analysisTable_->item(row, c))
                        analysisTable_->item(row, c)->setForeground(QColor(0xE7, 0x4C, 0x3C));
                }
            }
            ++row;
        }
        log("网络", QString("ANALYSIS 已更新 (%1 个传感器)").arg(row));
    } else if (type == "ALARM") {
        const QString level = msg.value("level").toString();
        const int sensor = msg.value("sensor").toInt();
        const QString name = (sensor >= 1 && sensor <= 4)
                                 ? QString::fromUtf8(kSensorNames[sensor - 1])
                                 : QString("传感器%1").arg(sensor);
        const QString text = QString("[%1][%2] %3: %4 (阈值 %5) %6")
                                 .arg(msg.value("ts").toString(), level, name,
                                      QString::number(msg.value("value").toDouble(), 'f', 1),
                                      QString::number(msg.value("threshold").toDouble(), 'f', 1),
                                      msg.value("msg").toString());
        auto* item = new QListWidgetItem(text);
        item->setForeground(level == "HIGH" ? QColor(0xE7, 0x4C, 0x3C)
                                            : QColor(0xE6, 0xA2, 0x3C));
        alarmList_->insertItem(0, item);   // 最新的显示在最上面
        log("网络", text);
        if (level == "HIGH" && !autoMode_) {
            /* 弹窗节流：同一传感器 60 秒内只弹一次。持续告警（如 MQ-2 抖动）
             * 场景下每条都弹模态框会把主窗口一直挡住（关一个又弹一个），
             * 导致界面无法操作/关闭。告警仍逐条进入上方告警列表，不丢失。 */
            const int idx = (sensor >= 1 && sensor <= 4) ? sensor : 0;
            const qint64 nowMs = clock_.elapsed();
            if (nowMs - lastHighPopupMs_[idx] >= 60000) {
                lastHighPopupMs_[idx] = nowMs;
                QMessageBox::warning(this, "⚠ 高级别告警", text);
            }
        }
    } else if (type == "ERROR") {
        const QString text = QString("[ERROR] code=%1 %2")
                                 .arg(msg.value("code").toInt())
                                 .arg(msg.value("msg").toString());
        alarmList_->insertItem(0, new QListWidgetItem(text));
        log("网络", text);
    }
}

// ---------------- 自动化验收模式 ----------------
void MainWindow::startAutoMode(const QString& port, int seconds, const QString& resultFile,
                               const QString& tcpHost, quint16 tcpPort) {
    autoMode_ = true;
    autoSeconds_ = seconds;
    autoResultFile_ = resultFile;
    autoTcpHost_ = tcpHost;
    autoTcpPort_ = tcpPort;
    log("系统", QString("自动验收模式: 串口=%1, 运行=%2s, TCP=%3:%4")
                    .arg(port).arg(seconds)
                    .arg(tcpHost.isEmpty() ? "无" : tcpHost)
                    .arg(tcpHost.isEmpty() ? 0 : tcpPort));
    if (!tcpHost.isEmpty())
        tcp_->connectTo(tcpHost, tcpPort, QStringLiteral("AUTO-CLIENT"));
    if (portBox_->findText(port) < 0)
        portBox_->addItem(port);
    portBox_->setCurrentText(port);
    toggleSerial();
    if (!port_.isOpen()) {
        QFile rf(resultFile);
        rf.open(QIODevice::WriteOnly | QIODevice::Text);
        rf.write(QString("AUTO_RESULT error=open_fail port=%1\n").arg(port).toUtf8());
        QApplication::quit();
        return;
    }
    QTimer::singleShot(seconds * 1000, this, &MainWindow::finishAuto);
}

void MainWindow::finishAuto() {
    qint64 points = 0;
    for (int i = 0; i < 4; ++i)
        points += plots_[i]->graph(0)->data()->size();
    port_.close();

    QStringList lastVals;
    for (int i = 0; i < 4; ++i) {
        lastVals << ((i == 0 || i == 1) ? QString::number(latest_[i], 'f', 1)
                                        : QString::number(latest_[i], 'f', 0));
    }
    QFile rf(autoResultFile_);
    rf.open(QIODevice::WriteOnly | QIODevice::Text);
    rf.write(QString(
                 "AUTO_RESULT frames=%1 dropped=%2 points=%3 uiTicks=%4 bytes=%5 "
                 "last=%6 run_seconds=%7\n")
                 .arg(parser_.validFrames())
                 .arg(parser_.droppedFrames())
                 .arg(points)
                 .arg(uiTicks_)
                 .arg(totalBytes_)
                 .arg(lastVals.join(';'))
                 .arg(autoSeconds_)
                 .toUtf8());
    if (tcp_) {
        rf.write(QString("tcp=%1 loginOk=%2 dataSent=%3 hbSent=%4 reconnects=%5 badJson=%6 "
                         "analysis=%7 alarm=%8\n")
                     .arg(tcp_->isLoggedIn() ? "LOGGED_IN" : "OFFLINE")
                     .arg(tcp_->loginOk())
                     .arg(tcp_->dataSent())
                     .arg(tcp_->hbSent())
                     .arg(tcp_->reconnects())
                     .arg(tcp_->badJson())
                     .arg(tcp_->analysisRcvd())
                     .arg(tcp_->alarmRcvd())
                     .toUtf8());
    }
    // 曲线数据诊断: 点数 / 键范围(epoch秒) / 值范围
    for (int i = 0; i < 4; ++i) {
        auto* d = plots_[i]->graph(0)->data().data();
        bool found = false;
        const QCPRange kr = d->keyRange(found);
        QCPRange vr(0, 0);
        if (found) vr = d->valueRange(found);
        rf.write(QString("curve%1 n=%2 key=[%3..%4] val=[%5..%6] yaxis=[%7..%8]\n")
                     .arg(i)
                     .arg(d->size())
                     .arg(kr.lower, 0, 'g', 12)
                     .arg(kr.upper, 0, 'g', 12)
                     .arg(vr.lower, 0, 'g', 12)
                     .arg(vr.upper, 0, 'g', 12)
                     .arg(plots_[i]->yAxis->range().lower, 0, 'g', 8)
                     .arg(plots_[i]->yAxis->range().upper, 0, 'g', 8)
                     .toUtf8());
    }
    QApplication::quit();
}
