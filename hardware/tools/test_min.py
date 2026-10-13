# -*- coding: utf-8 -*-
"""最小实验：验证 KiCad 10.0.6 能否解析‘库语法内嵌符号’的 sch 文件。"""
import sys, os, uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexpr import SymbolLib, rename_symbol, strip_new_fields, indent

SYM = r"D:\KiCad\10.0.6\share\kicad\symbols"
OUT = r"C:\Users\15373\AppData\Local\Temp\kicad_exp"
os.makedirs(OUT, exist_ok=True)

dev = SymbolLib(os.path.join(SYM, "Device.kicad_sym"))
pwr = SymbolLib(os.path.join(SYM, "power.kicad_sym"))

def uid():
    return str(uuid.uuid4())

def sym_instance(lib_id, ref, value, x, y, angle, pins, root_uuid, extra_props=()):
    lines = []
    lines.append("\t(symbol")
    lines.append(f'\t\t(lib_id "{lib_id}")')
    lines.append(f"\t\t(at {x} {y} {angle})")
    lines.append("\t\t(unit 1)")
    lines.append("\t\t(exclude_from_sim no)")
    lines.append("\t\t(in_bom yes)")
    lines.append("\t\t(on_board yes)")
    lines.append("\t\t(dnp no)")
    lines.append(f'\t\t(uuid "{uid()}")')
    for name, val, px, py, hide in [(("Reference"), ref, x + 2.54, y - 1.27, False),
                                    (("Value"), value, x + 2.54, y + 1.27, False)] + list(extra_props):
        h = "\n\t\t\t\t(hide yes)" if hide else ""
        lines.append(f'\t\t(property "{name}" "{val}"')
        lines.append(f"\t\t\t(at {px} {py} 0)")
        lines.append("\t\t\t(effects")
        lines.append("\t\t\t\t(font")
        lines.append("\t\t\t\t\t(size 1.27 1.27)")
        lines.append("\t\t\t\t)" + h)
        lines.append("\t\t\t)")
        lines.append("\t\t)")
    for p in pins:
        lines.append(f'\t\t(pin "{p}"')
        lines.append(f'\t\t\t(uuid "{uid()}")')
        lines.append("\t\t)")
    lines.append("\t\t(instances")
    lines.append('\t\t\t(project ""')
    lines.append(f'\t\t\t\t(path "/{root_uuid}"')
    lines.append(f'\t\t\t\t\t(reference "{ref}")')
    lines.append("\t\t\t\t\t(unit 1)")
    lines.append("\t\t\t\t)")
    lines.append("\t\t\t)")
    lines.append("\t\t)")
    lines.append("\t)")
    return "\n".join(lines)

def build(version, generator_version, strip):
    root = uid()
    parts = []
    parts.append("(kicad_sch")
    parts.append(f"\t(version {version})")
    parts.append('\t(generator "eeschema")')
    parts.append(f'\t(generator_version "{generator_version}")')
    parts.append(f'\t(uuid "{root}")')
    parts.append('\t(paper "A4")')

    # ---- lib_symbols ----
    libsyms = []
    r_blk = dev.get("R")
    gnd_blk = pwr.get("GND")
    if strip:
        r_blk = strip_new_fields(r_blk)
        gnd_blk = strip_new_fields(gnd_blk)
    libsyms.append(rename_symbol(r_blk, "Device:R"))
    libsyms.append(rename_symbol(gnd_blk, "power:GND"))
    parts.append("\t(lib_symbols")
    for b in libsyms:
        parts.append(indent(b, 2))
    parts.append("\t)")

    # ---- 元件实例 ----
    # R 在 (100, 100)，角度 0（引脚1在上方 -3.81，引脚2在下方 +3.81）
    parts.append(sym_instance("Device:R", "R1", "10k", 100, 100, 0, ["1", "2"], root))
    # GND 在 (124.46, 96.52) ？先放在 R 正下方: GND 符号原点即引脚点
    parts.append(sym_instance("power:GND", "#PWR01", "GND", 100, 103.81, 0, ["1"], root))

    # 一根线连接 R 引脚2(100,103.81) 到 GND(100,103.81) 无需线；换个: 从 R 引脚1 往上拉
    parts.append("\t(wire")
    parts.append("\t\t(pts")
    parts.append("\t\t\t(xy 100 96.19) (xy 100 92.71)")
    parts.append("\t\t)")
    parts.append("\t\t(stroke")
    parts.append("\t\t\t(width 0)")
    parts.append("\t\t\t(type default)")
    parts.append("\t\t)")
    parts.append(f'\t\t(uuid "{uid()}")')
    parts.append("\t)")
    parts.append('\t(label "VIN"')
    parts.append("\t\t(at 100 92.71 0)")
    parts.append("\t\t(effects")
    parts.append("\t\t\t(font")
    parts.append("\t\t\t\t(size 1.27 1.27)")
    parts.append("\t\t\t)")
    parts.append("\t\t\t(justify left bottom)")
    parts.append("\t\t)")
    parts.append(f'\t\t(uuid "{uid()}")')
    parts.append("\t)")

    parts.append("\t(sheet_instances")
    parts.append('\t\t(path "/"')
    parts.append('\t\t\t(page "1")')
    parts.append("\t\t)")
    parts.append("\t)")
    parts.append('\t(embedded_fonts no)')
    parts.append(")")
    return "\n".join(parts) + "\n"

for tag, ver, gv, strip in [("A_lib_syntax", 20251024, "10.0", False),
                            ("B_old_syntax", 20250114, "9.0", True)]:
    path = os.path.join(OUT, f"test_{tag}.kicad_sch")
    with open(path, "w", encoding="utf-8") as f:
        f.write(build(ver, gv, strip))
    print("written:", path)
