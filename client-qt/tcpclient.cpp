// tcpclient.cpp —— B4 TCP 网络模块实现
#include "tcpclient.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTcpSocket>

namespace {
constexpr int kHeartbeatSecs = 30;     // 接口规范 §3: 心跳 30s/次
constexpr int kBackoffSecs[4] = {5, 10, 20, 30};  // 退避 5/10/20/30s 封顶
constexpr int kMaxLine = 4096;         // 单条消息 ≤ 4KB
}  // namespace

TcpClient::TcpClient(QObject* parent) : QObject(parent) {
    sock_ = new QTcpSocket(this);
    connect(sock_, &QTcpSocket::connected, this, &TcpClient::onConnected);
    connect(sock_, &QTcpSocket::disconnected, this, &TcpClient::onDisconnected);
    connect(sock_, &QTcpSocket::readyRead, this, &TcpClient::onReadyRead);
    connect(sock_, &QTcpSocket::errorOccurred, this, &TcpClient::onError);

    heartbeat_ = new QTimer(this);
    heartbeat_->setInterval(kHeartbeatSecs * 1000);
    connect(heartbeat_, &QTimer::timeout, this, &TcpClient::onHeartbeat);

    reconnect_ = new QTimer(this);
    reconnect_->setSingleShot(true);
    connect(reconnect_, &QTimer::timeout, this, &TcpClient::onReconnect);
}

void TcpClient::connectTo(const QString& host, quint16 port, const QString& clientId) {
    userStop_ = false;
    backoffIdx_ = 0;
    host_ = host;
    port_ = port;
    clientId_ = clientId;
    reconnect_->stop();
    state_ = State::Connecting;
    emit stateChanged("● 连接中…", "#E6A23C");
    emit logEvent(QString("连接 %1:%2 (client_id=%3)").arg(host).arg(port).arg(clientId));
    sock_->abort();
    sock_->connectToHost(host, port);
}

void TcpClient::stop() {
    userStop_ = true;
    clearTimers();
    state_ = State::Idle;
    loggedIn_ = false;
    if (sock_->state() != QAbstractSocket::UnconnectedState)
        sock_->abort();
    emit stateChanged("● 未连接", "#888888");
    emit logEvent("已手动断开");
}

void TcpClient::sendJson(const QJsonObject& obj) {
    if (sock_->state() != QAbstractSocket::ConnectedState) return;
    const QByteArray frame = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > kMaxLine) {
        emit logEvent(QString("消息超长(%1B)已丢弃").arg(frame.size()));
        return;
    }
    sock_->write(frame);
}

void TcpClient::sendData(const QJsonArray& sensors) {
    QJsonObject o;
    o.insert("type", "DATA");
    o.insert("ts", QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    o.insert("sensors", sensors);
    sendJson(o);
    if (state_ == State::LoggedIn) ++dataSent_;
}

void TcpClient::onConnected() {
    state_ = State::LoggingIn;
    emit stateChanged("● 已连接, 登录中…", "#E6A23C");
    emit logEvent(QString("TCP 已连接 %1:%2, 发送 LOGIN").arg(host_).arg(port_));
    QJsonObject o;
    o.insert("type", "LOGIN");
    o.insert("client_id", clientId_);
    sendJson(o);   // 连接建立后第一条消息
}

void TcpClient::onDisconnected() {
    heartbeat_->stop();
    if (loggedIn_)
        emit logEvent("连接断开");
    loggedIn_ = false;
    if (userStop_) return;
    if (!reconnect_->isActive())          // 与 onError 去重
        scheduleReconnect();
}

void TcpClient::onError() {
    if (userStop_) return;
    emit logEvent(QString("网络错误: %1").arg(sock_->errorString()));
    // 连接失败/网络错误: 与 onDisconnected 去重, 只调度一次退避重连
    if (!reconnect_->isActive() && sock_->state() == QAbstractSocket::UnconnectedState)
        scheduleReconnect();
}

void TcpClient::onReadyRead() {
    buf_ += sock_->readAll();
    if (buf_.size() > 64 * kMaxLine) {   // 防御: 缓冲异常膨胀
        emit logEvent("接收缓冲异常, 重置连接");
        sock_->abort();
        return;
    }
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        const QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            handleLine(line);
    }
}

void TcpClient::handleLine(const QByteArray& line) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (doc.isNull() || !doc.isObject()) {
        ++badJson_;
        emit logEvent(QString("非法JSON: %1").arg(err.errorString()));
        return;
    }
    const QJsonObject msg = doc.object();
    const QString type = msg.value("type").toString();

    if (type == "LOGIN_ACK") {
        const int status = msg.value("status").toInt(-1);
        if (status == 0) {
            state_ = State::LoggedIn;
            loggedIn_ = true;
            backoffIdx_ = 0;               // 重连成功, 退避复位
            ++loginOk_;
            heartbeat_->start();           // 登录成功后启动 30s 心跳
            emit stateChanged("● 已登录", "#27AE60");
            emit logEvent(QString("LOGIN_ACK status=0, 已登录 (#%1)").arg(loginOk_));
        } else {
            emit logEvent(QString("LOGIN 被拒绝 status=%1").arg(status));
            sock_->abort();                // 触发退避重连
        }
    } else if (type == "HEARTBEAT_ACK") {
        // 心跳应答, 静默
    } else if (type == "ANALYSIS") {
        ++analysisRcvd_;
        emit jsonReceived(msg);
    } else if (type == "ALARM") {
        ++alarmRcvd_;
        emit jsonReceived(msg);
    } else if (type == "ERROR") {
        emit jsonReceived(msg);
    } else {
        emit logEvent(QString("未知消息类型: %1").arg(type));
    }
}

void TcpClient::onHeartbeat() {
    if (state_ != State::LoggedIn) return;
    QJsonObject o;
    o.insert("type", "HEARTBEAT");
    sendJson(o);
    ++hbSent_;
    emit logEvent(QString("HEARTBEAT #%1").arg(hbSent_));
}

void TcpClient::scheduleReconnect() {
    state_ = State::Backoff;
    const int delay = kBackoffSecs[qBound(0, backoffIdx_, 3)];
    ++backoffIdx_;
    ++reconnects_;
    emit stateChanged(QString("● 断线, %1s后重连…").arg(delay), "#E6A23C");
    emit logEvent(QString("断线, 第%1次退避: %2s后重连").arg(reconnects_).arg(delay));
    reconnect_->start(delay * 1000);
}

void TcpClient::onReconnect() {
    if (userStop_ || state_ != State::Backoff) return;
    state_ = State::Connecting;
    emit stateChanged("● 重连中…", "#E6A23C");
    emit logEvent(QString("重连 %1:%2 …").arg(host_).arg(port_));
    sock_->abort();
    sock_->connectToHost(host_, port_);
}

void TcpClient::clearTimers() {
    heartbeat_->stop();
    reconnect_->stop();
}

void TcpClient::setState(State s) { state_ = s; }  // 预留
