# -*- coding: utf-8 -*-
"""检查 .kicad_sch 的括号配平，打印出错位置。"""
import sys


def check(path):
    with open(path, encoding="utf-8") as f:
        t = f.read()
    depth = 0
    in_str = False
    line = 1
    esc = False
    for ch in t:
        if ch == "\n":
            line += 1
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        else:
            if ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth < 0:
                    print(f"!! 第 {line} 行括号多余")
                    return False
    print(f"final depth={depth} in_str={in_str}")
    return depth == 0 and not in_str


if __name__ == "__main__":
    p = sys.argv[1]
    ok = check(p)
    print("OK" if ok else "BAD")
    if not ok and len(sys.argv) > 2:
        # 打印出错行附近内容
        with open(p, encoding="utf-8") as f:
            lines = f.readlines()
        n = int(sys.argv[2])
        for i in range(max(0, n - 6), min(len(lines), n + 6)):
            print(f"{i+1:6}: {lines[i].rstrip()}")
