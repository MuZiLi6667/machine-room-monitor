# -*- coding: utf-8 -*-
r"""S-expression 工具：从 KiCad 官方符号库(.kicad_sym)中提取符号定义块。

用法:
    from sexpr import SymbolLib
    lib = SymbolLib(r"D:\KiCad\10.0.6\share\kicad\symbols\Device.kicad_sym")
    block = lib.get("R")          # 返回符号的完整文本块（含首尾括号）
"""
import re


def _find_block(text: str, start: int) -> str:
    """从 text[start] 处的 '(' 开始，返回括号配平的完整块。"""
    assert text[start] == "(", f"expect '(' at {start}, got {text[start]!r}"
    depth = 0
    in_string = False
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if in_string:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_string = False
        else:
            if c == '"':
                in_string = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    return text[start : i + 1]
        i += 1
    raise ValueError("unbalanced parentheses")


class SymbolLib:
    """一个 .kicad_sym 库文件，可按名字提取顶层符号。"""

    def __init__(self, path: str):
        self.path = path
        with open(path, "r", encoding="utf-8") as f:
            self.text = f.read()
        # 顶层符号：行首一个 tab 缩进 + (symbol "NAME"
        self._tops = {}
        for m in re.finditer(r'^\t\(symbol "([^"]+)"', self.text, re.M):
            self._tops[m.group(1)] = m.start() + 1  # +1 跳过行首的 tab，指向 '('

    def names(self):
        return sorted(self._tops)

    def get(self, name: str) -> str:
        if name not in self._tops:
            raise KeyError(f"symbol {name!r} not found in {self.path}")
        return _find_block(self.text, self._tops[name])

    def get_header(self) -> str:
        """返回库文件头 (kicad_symbol_lib (version ...) (generator ...))。"""
        return _find_block(self.text, 0)

    def version(self) -> str:
        m = re.search(r"\(version (\d+)\)", self.text[:400])
        return m.group(1) if m else "?"


def strip_new_fields(block: str) -> str:
    """把库(2025+)的新语法字段剥掉，转成 sch 内嵌语法(20250114 一档)。

    移除: (show_name no), (do_not_autoplace no), (in_pos_files yes),
          (duplicate_pin_numbers_are_jumpers no)
    并把属性里独立成行的 (hide yes) 转为 effects 内的 (hide yes)。
    """
    out = block
    for pat in (r"\t*\(show_name no\)\n", r"\t*\(do_not_autoplace no\)\n",
                r"\t*\(in_pos_files yes\)\n",
                r"\t*\(duplicate_pin_numbers_are_jumpers no\)\n"):
        out = re.sub(pat, "", out)
    return out


def rename_symbol(block: str, new_name: str) -> str:
    """把顶层符号名替换为 new_name（只替换第一处）。"""
    return re.sub(r'\(symbol "([^"]+)"', '(symbol "%s"' % new_name, block, count=1)


def indent(block: str, tabs: int) -> str:
    pad = "\t" * tabs
    return "\n".join(pad + line if line.strip() else line for line in block.splitlines())
