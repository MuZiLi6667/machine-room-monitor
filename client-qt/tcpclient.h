// tcpclient.h —— B4 TCP 网络模块（接口规范 §3）
// 协议: JSON + '\n' 分帧(UTF-8, 单条≤4KB)
// 客户端: 连接后首条 LOGIN; 30s 心跳 HEARTBEAT; DATA 转发由上层调用 sendData()
// 断线: socket error/对端关闭 → 退避重连 5/10/20/30s 封顶, 重连成功后重新 LOGIN
// 服务器下行: LOGIN_ACK/HEARTBEAT_ACK 内部消化, ANALYSIS/ALARM/ERROR 经 jsonReceived 上抛
#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QTcpSocket;

class TcpClient : public QObject {
    Q_OBJECT
public:
    explicit TcpClient(QObject* parent = nullptr);

    void connectTo(const QString& host, quint16 port, const QString& clientId);
    void stop();                      // 用户主动断开(不重连)
    bool isLoggedIn() const { return loggedIn_; }
    bool busy() const { return state_ != State::Idle; }  // 连接中/登录中/已登录/退避重连中

    // DATA 转发: sensors = [{"id":1,"v":25.6}, ...]
    void sendData(const class QJsonArray& sensors);
    void sendJson(const class QJsonObject& obj);

    // 验收统计
    long hbSent() const { return hbSent_; }
    long dataSent() const { return dataSent_; }
    long loginOk() const { return loginOk_; }
    long reconnects() const { return reconnects_; }
    long badJson() const { return badJson_; }
    long analysisRcvd() const { return analysisRcvd_; }
    long alarmRcvd() const { return alarmRcvd_; }

signals:
    void stateChanged(const QString& text, const QString& color);
    void jsonReceived(const class QJsonObject& msg);   // ANALYSIS/ALARM/ERROR 等
    void logEvent(const QString& text);                // 网络日志(上层按开关过滤)

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError();
    void onHeartbeat();
    void onReconnect();

private:
    enum class State { Idle, Connecting, LoggingIn, LoggedIn, Backoff };

    void setState(State s);
    void scheduleReconnect();
    void handleLine(const QByteArray& line);
    void clearTimers();

    QTcpSocket* sock_ = nullptr;
    State state_ = State::Idle;
    QString host_;
    quint16 port_ = 0;
    QString clientId_;
    QByteArray buf_;

    QTimer* heartbeat_ = nullptr;    // 30s 周期
    QTimer* reconnect_ = nullptr;    // 单次退避定时
    int backoffIdx_ = 0;             // 0..3 → 5/10/20/30s, 封顶 30
    bool userStop_ = false;
    bool loggedIn_ = false;

    // 统计
    long hbSent_ = 0;
    long dataSent_ = 0;
    long loginOk_ = 0;
    long reconnects_ = 0;
    long badJson_ = 0;
    long analysisRcvd_ = 0;
    long alarmRcvd_ = 0;
};

#endif  // TCPCLIENT_H
