# -*- coding: utf-8 -*-
"""实验2：内嵌 STM32F103C8Tx（extends 派生符号）并实例化，验证解析。"""
import sys, os, re, uuid
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexpr import SymbolLib, rename_symbol, indent

SYM = r"D:\KiCad\10.0.6\share\kicad\symbols"
OUT = r"C:\Users\15373\AppData\Local\Temp\kicad_exp"
os.makedirs(OUT, exist_ok=True)

def uid():
    return str(uuid.uuid4())

mcu = SymbolLib(os.path.join(SYM, "MCU_ST_STM32F1.kicad_sym"))
base_blk = mcu.get("STM32F103C_8-B_Tx")
der_blk = mcu.get("STM32F103C8Tx")
pins = re.findall(r'\(number "([^"]+)"', base_blk)
print("引脚数量:", len(pins), "前几个:", pins[:8])

def instance(lib_id, ref, value, x, y, angle, pins):
    root = ROOT
    L = []
    L.append("\t(symbol")
    L.append(f'\t\t(lib_id "{lib_id}")')
    L.append(f"\t\t(at {x} {y} {angle})")
    L.append("\t\t(unit 1)")
    L.append("\t\t(exclude_from_sim no)")
    L.append("\t\t(in_bom yes)")
    L.append("\t\t(on_board yes)")
    L.append("\t\t(dnp no)")
    L.append(f'\t\t(uuid "{uid()}")')
    L.append(f'\t\t(property "Reference" "{ref}"')
    L.append(f"\t\t\t(at {x} {y-35} 0)")
    L.append("\t\t\t(effects")
    L.append("\t\t\t\t(font")
    L.append("\t\t\t\t\t(size 1.27 1.27)")
    L.append("\t\t\t\t)")
    L.append("\t\t\t)")
    L.append("\t\t)")
    L.append(f'\t\t(property "Value" "{value}"')
    L.append(f"\t\t\t(at {x} {y-32} 0)")
    L.append("\t\t\t(effects")
    L.append("\t\t\t\t(font")
    L.append("\t\t\t\t\t(size 1.27 1.27)")
    L.append("\t\t\t\t)")
    L.append("\t\t\t)")
    L.append("\t\t)")
    L.append('\t\t(property "Footprint" ""')
    L.append(f"\t\t\t(at {x} {y} 0)")
    L.append("\t\t\t(effects")
    L.append("\t\t\t\t(font (size 1.27 1.27))")
    L.append("\t\t\t\t(hide yes)")
    L.append("\t\t\t)")
    L.append("\t\t)")
    for p in pins:
        L.append(f'\t\t(pin "{p}"')
        L.append(f'\t\t\t(uuid "{uid()}")')
        L.append("\t\t)")
    L.append("\t\t(instances")
    L.append('\t\t\t(project ""')
    L.append(f'\t\t\t\t(path "/{root}"')
    L.append(f'\t\t\t\t\t(reference "{ref}")')
    L.append("\t\t\t\t\t(unit 1)")
    L.append("\t\t\t\t)")
    L.append("\t\t\t)")
    L.append("\t\t)")
    L.append("\t)")
    return "\n".join(L)

ROOT = uid()
parts = []
parts.append("(kicad_sch")
parts.append("\t(version 20251024)")
parts.append('\t(generator "eeschema")')
parts.append('\t(generator_version "10.0")')
parts.append(f'\t(uuid "{ROOT}")')
parts.append('\t(paper "A4")')
parts.append("\t(lib_symbols")
parts.append(indent(rename_symbol(base_blk, "MCU_ST_STM32F1:STM32F103C_8-B_Tx"), 2))
parts.append(indent(rename_symbol(der_blk, "MCU_ST_STM32F1:STM32F103C8Tx"), 2))
parts.append("\t)")
parts.append(instance("MCU_ST_STM32F1:STM32F103C8Tx", "U1", "STM32F103C8Tx", 100, 100, 0, pins))
parts.append("\t(sheet_instances")
parts.append('\t\t(path "/"')
parts.append('\t\t\t(page "1")')
parts.append("\t\t)")
parts.append("\t)")
parts.append('\t(embedded_fonts no)')
parts.append(")")

path = os.path.join(OUT, "test_stm32.kicad_sch")
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(parts) + "\n")
print("written:", path)
