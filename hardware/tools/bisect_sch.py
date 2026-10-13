# -*- coding: utf-8 -*-
"""二分定位：找出 sch 文件中导致 KiCad 解析失败的顶层块。

用法: python bisect_sch.py <file.kicad_sch>
先单独测"只保留 lib_symbols"，再逐个加入后面每个顶层块，定位第一个出错的块。
"""
import os
import subprocess
import sys

CLI = r"D:\KiCad\10.0.6\bin\kicad-cli.exe"
TMP = r"C:\Users\15373\AppData\Local\Temp\kicad_exp\bisect"
os.makedirs(TMP, exist_ok=True)


def top_blocks(text):
    """把 (kicad_sch ...) 的第一层子块切成文本块列表。"""
    i = text.index("(")
    depth = 0
    in_str = False
    esc = False
    blocks = []
    start = None
    for idx in range(i, len(text)):
        c = text[idx]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == "(":
            depth += 1
            if depth == 2:
                start = idx
        elif c == ")":
            if depth == 2 and start is not None:
                blocks.append(text[start:idx + 1])
                start = None
            depth -= 1
    return blocks


def head_of(block):
    return block.strip().split("\n")[0][:70]


def try_load(parts, name):
    p = os.path.join(TMP, name + ".kicad_sch")
    with open(p, "w", encoding="utf-8") as f:
        f.write("(kicad_sch\n")
        f.write("\n".join(parts))
        f.write("\n)\n")
    r = subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                        "-o", os.path.join(TMP, name + ".net"), p],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    ok = "失败" not in (r.stdout or "") and r.returncode == 0 and "失败" not in (r.stderr or "")
    return ok, (r.stdout or "") + (r.stderr or "")


if __name__ == "__main__":
    path = sys.argv[1]
    text = open(path, encoding="utf-8").read()
    blocks = top_blocks(text)
    print(f"共 {len(blocks)} 个顶层块")
    # 基础 = 只保留 lib_symbols（第 0 块是 version? 不，第一层是 version/generator/uuid/paper/...）
    # 找到 lib_symbols 块下标
    lib_idx = next(i for i, b in enumerate(blocks) if b.strip().startswith("(lib_symbols"))
    header = blocks[:lib_idx + 1]
    ok, msg = try_load(header, "t_head")
    print(f"[header+lib_symbols] {'OK' if ok else 'FAIL'}  {msg.strip()[:100]}")
    if not ok:
        sys.exit(0)
    acc = list(header)
    for i in range(lib_idx + 1, len(blocks)):
        acc2 = acc + [blocks[i]]
        ok, msg = try_load(acc2, f"t_{i}")
        tag = "OK  " if ok else "FAIL"
        print(f"[+块{i:03d}] {tag} {head_of(blocks[i])}")
        if not ok:
            print("  错误:", msg.strip()[:300])
            break
        acc = acc2
