#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TCP 探测工具（双模式）—— 按接口规范 §3 收发 JSON 消息
  模式1 假服务器：给客户端（任务B）当对端，验证客户端的登录/转发/重连
      python server_probe.py server -p 9000
  模式2 假客户端：给服务器（任务C）当对端，登录并发送2秒周期的 DATA，打印收到的 ANALYSIS/ALARM
      python server_probe.py client -H <服务器IP> -p 9000 --id PC-01
依赖：仅标准库
"""
import argparse
import json
import random
import socket
import sys
import threading
import time


def jsonline(obj):
    return (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")


def run_server(args):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", args.port))
    srv.listen(1)
    print(f"[probe-server] listening on 0.0.0.0:{args.port}, waiting for client...")
    while True:
        conn, addr = srv.accept()
        print(f"[+] client connected from {addr}")
        buf = b""
        with conn:
            while True:
                try:
                    chunk = conn.recv(4096)
                except (ConnectionResetError, OSError):
                    break
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        conn.sendall(jsonline({"type": "ERROR", "code": 1, "msg": "malformed json"}))
                        continue
                    t = msg.get("type")
                    if t == "LOGIN":
                        conn.sendall(jsonline({"type": "LOGIN_ACK", "status": 0}))
                        print(f"[LOGIN] {msg.get('client_id')}")
                    elif t == "HEARTBEAT":
                        conn.sendall(jsonline({"type": "HEARTBEAT_ACK"}))
                        print("[HEARTBEAT] -> ACK")
                    elif t == "DATA":
                        print(f"[DATA] {line.decode('utf-8', 'replace')}")
                    else:
                        conn.sendall(jsonline({"type": "ERROR", "code": 2, "msg": "unknown type"}))
        print("[-] client disconnected, waiting for reconnect...")


def run_client(args):
    while True:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=5)
        except OSError as e:
            print(f"[!] connect to {args.host}:{args.port} failed: {e} — retry in 5s")
            time.sleep(5)
            continue
        print(f"[+] connected to {args.host}:{args.port}")
        with sock:
            sock.settimeout(120)

            def reader():
                buf = b""
                while True:
                    try:
                        chunk = sock.recv(4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if line.strip():
                            print(f"[<-] {line.decode('utf-8', 'replace')}")

            threading.Thread(target=reader, daemon=True).start()
            try:
                sock.sendall(jsonline({"type": "LOGIN", "client_id": args.id}))
                n = 0
                while True:
                    n += 1
                    data = {
                        "type": "DATA",
                        "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                        "sensors": [
                            {"id": 1, "v": round(random.uniform(20, 32), 1)},
                            {"id": 2, "v": round(random.uniform(40, 70), 1)},
                            {"id": 3, "v": random.randint(80, 450)},
                            {"id": 4, "v": random.randint(100, 600)},
                        ],
                    }
                    sock.sendall(jsonline(data))
                    print(f"[-> #{n}] {json.dumps(data, ensure_ascii=False)}")
                    time.sleep(args.interval)
            except OSError as e:
                print(f"[!] connection lost: {e} — reconnecting in 5s")
                time.sleep(5)


def main():
    ap = argparse.ArgumentParser(description="TCP 探测工具（假服务器/假客户端）")
    sub = ap.add_subparsers(dest="mode", required=True)

    ps = sub.add_parser("server", help="假服务器模式（测客户端）")
    ps.add_argument("-p", "--port", type=int, default=9000)

    pc = sub.add_parser("client", help="假客户端模式（测服务器）")
    pc.add_argument("-H", "--host", default="127.0.0.1")
    pc.add_argument("-p", "--port", type=int, default=9000)
    pc.add_argument("--id", default="PROBE-01")
    pc.add_argument("-i", "--interval", type=float, default=2.0)

    args = ap.parse_args()
    try:
        run_server(args) if args.mode == "server" else run_client(args)
    except KeyboardInterrupt:
        print("\n[probe] stopped")


if __name__ == "__main__":
    main()
