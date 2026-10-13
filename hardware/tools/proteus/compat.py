# -*- coding: utf-8 -*-
"""让 proteus-automatic-api 适配本机 Proteus 8.16 库格式。

差异 1: pinout 行可能是 `PIN VDDA = 9}`（缺开头的 `{` 且名字无引号）。
差异 2: pinout 里有 `{NC=n}` 行（无引脚名的空焊盘），包的解析器不认识。
差异 3: 包的默认库路径/模板路径与本机安装位置不同。
"""
import re

import device_library

LIB_DIR = r"C:\ProgramData\Labcenter Electronics\Proteus 8 Professional\LIBRARY"
TEMPLATE = (r"C:\ProgramData\Labcenter Electronics\Proteus 8 Professional"
            r"\SAMPLES\Graph Based Simulation\Rescap.pdsprj")
PDS_EXE = (r"C:\Program Files (x86)\Labcenter Electronics\Proteus 8 Professional"
           r"\BIN\PDS.EXE")


def _install():
    # ---------- 1. 默认库路径 ----------
    _orig_init = device_library.Library.__init__

    def patched_init(self, directory=LIB_DIR):
        _orig_init(self, directory)

    device_library.Library.__init__ = patched_init

    # ---------- 2. 规范化 pinout 行 ----------
    _orig_sections = device_library.property_sections

    def patched_sections(raw):
        fixed = []
        for line in raw.splitlines():
            s = line.strip()
            if s:
                # 补开头的 '{'
                if not s.startswith("{") and re.fullmatch(r"(?:PIN|PP)\b.*\}", s):
                    s = "{" + s
                # 名字补引号: {PIN NAME = n} -> {PIN "NAME" = n}
                m = re.fullmatch(r'\{PIN\s+([^"{}]+?)\s*=\s*([\w/*]+)\}', s)
                if m:
                    s = '{{PIN "{}" = {}}}'.format(m.group(1).strip(), m.group(2))
            fixed.append(s)
        return _orig_sections("\n".join(fixed))

    device_library.property_sections = patched_sections

    # ---------- 3. 过滤 {NC=n} 行 + 非 ASCII 属性转 ASCII ----------
    _orig_describe = device_library.describe_definition

    def _ascii(block):
        out = []
        for l in block:
            s = l.replace("°", "deg").replace("µ", "u")
            s = s.encode("ascii", "replace").decode("ascii")
            out.append(s)
        return out

    def patched_describe(entry, body):
        info = _orig_describe(entry, body)
        _skip = re.compile(r"\{(?:NC|PINSWAP)\s*=[^}]*\}")
        for pkg, lines in info["pinouts"].items():
            info["pinouts"][pkg] = [l for l in lines if not _skip.fullmatch(l.strip())]
        for sec, lines in list(info["sections"].items()):
            if not all(l.isascii() for l in lines):
                info["sections"][sec] = _ascii(lines)
        return info

    device_library.describe_definition = patched_describe


_install()
