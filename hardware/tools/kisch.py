# -*- coding: utf-8 -*-
r"""kisch —— 用 Python 生成 KiCad 原理图(.kicad_sch)的迷你库。

思路：从 KiCad 官方符号库提取符号定义（内嵌到 sch 的 lib_symbols，自包含、
不依赖用户机器上的库配置），再放置元件实例、画线、加标签。

坐标约定：sch 文件里 X 向右、Y 向下为正；符号库内部 Y 向上为正。
"""
import os
import re
import uuid

from sexpr import SymbolLib, rename_symbol, indent, _find_block


def uid() -> str:
    return str(uuid.uuid4())


def fmt(v: float) -> str:
    """KiCad 风格的数字格式化：去掉多余小数位。"""
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


class LibSymbol:
    """一个内嵌到 sch 里的符号定义。"""

    def __init__(self, lib_id: str, block: str, pins: dict, extends: str | None):
        self.lib_id = lib_id          # 例如 "Device:R"
        self.block = block            # 完整 S-expr 文本（名字已改为 lib_id）
        self.pins = pins              # {pin_number: (x, y, angle)}
        self.extends = extends        # 基符号名（不带库前缀）或 None


class Component:
    """一个放置到图纸上的元件实例。"""

    def __init__(self, sch, lib_id, ref, value, x, y, angle, mirror, uu):
        self.sch = sch
        self.lib_id = lib_id
        self.ref = ref
        self.value = value
        self.x, self.y, self.angle = x, y, angle
        self.mirror = mirror          # None / "x" / "y"
        self.uuid = uu

    def pin(self, number: str):
        """返回引脚连接点的世界坐标 (x, y)。"""
        sym = self.sch.symbols[self.lib_id]
        px, py, _ = sym.pins[str(number)]
        return self.sch.transform(px, py, self.x, self.y, self.angle, self.mirror)


class Sch:
    SYMDIR = r"D:\KiCad\10.0.6\share\kicad\symbols"

    def __init__(self, paper="A3", title="", date="2026-10-09", rev="A",
                 company="", comments=()):
        self.paper = paper
        self.title = title
        self.date = date
        self.rev = rev
        self.company = company
        self.comments = list(comments)
        self.uuid = uid()
        self.symbols: dict[str, LibSymbol] = {}
        self.items = []               # 图纸上的图元（文本行列表）
        self._libs: dict[str, SymbolLib] = {}
        self.project = "machine-room-monitor"

    # ---------- 旋转/镜像变换（由标定实验确定） ----------
    @staticmethod
    def transform(px, py, x0, y0, angle, mirror=None):
        """符号库内坐标 (px,py) → 图纸世界坐标。"""
        # 库坐标 Y 向上，先翻转成图纸相对坐标
        rx, ry = px, -py
        if mirror == "x":          # 沿 X 轴镜像
            ry = -ry
        elif mirror == "y":
            rx = -rx
        a = angle % 360
        if a == 0:
            dx, dy = rx, ry
        elif a == 90:
            dx, dy = ry, rx
        elif a == 180:
            dx, dy = -rx, -ry
        else:  # 270
            dx, dy = -ry, -rx
        return (round(x0 + dx, 4), round(y0 + dy, 4))

    # ---------- 符号库 ----------
    def _lib(self, name: str) -> SymbolLib:
        if name not in self._libs:
            self._libs[name] = SymbolLib(os.path.join(self.SYMDIR, name + ".kicad_sym"))
        return self._libs[name]

    def _collect_pins(self, blk: str) -> dict:
        """从符号块(或它的基符号块)收集引脚 {number: (x,y,angle)}。"""
        pins = {}
        for m in re.finditer(
                r'\(pin\s+\S+\s+\S+\s*\(at\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)'
                r'.*?\(number\s+"([^"]+)"', blk, re.S):
            pins[m.group(4)] = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
        return pins

    @staticmethod
    def _props_of(blk: str, tabs: int = 2) -> dict:
        """提取符号块里的顶层属性 {属性名: 属性块文本}。"""
        out = {}
        pat = r'^\t{%d}\(property "([^"]+)"' % tabs
        for m in re.finditer(pat, blk, re.M):
            start = m.start() + tabs
            out[m.group(1)] = _find_block(blk, start)
        return out

    @classmethod
    def _flatten(cls, base_blk: str, derived_blk: str,
                 base_name: str, derived_name: str) -> str:
        """把 extends 派生符号扁平化：基符号的图形/引脚 + 派生符号的属性。

        KiCad 保存 sch 时也做同样的扁平化；否则实例可能被静默丢弃。
        """
        base_props = cls._props_of(base_blk)
        der_props = cls._props_of(derived_blk)
        dst = base_blk
        for k, v in der_props.items():
            if k in base_props:
                dst = dst.replace(base_props[k], v, 1)
        # 把基符号的子单元名改成派生符号的子单元名
        dst = dst.replace(f'(symbol "{base_name}_', f'(symbol "{derived_name}_')
        return dst

    def load(self, lib: str, name: str):
        """从官方库加载符号（派生符号自动扁平化），内嵌到本图纸。返回 lib_id。"""
        symlib = self._lib(lib)
        blk = symlib.get(name)
        m = re.search(r'\(extends\s+"([^"]+)"', blk)
        if m:
            base_name = m.group(1)
            blk = self._flatten(symlib.get(base_name), blk, base_name, name)
        pins = self._collect_pins(blk)
        lib_id = f"{lib}:{name}"
        self.symbols[lib_id] = LibSymbol(
            lib_id, rename_symbol(blk, lib_id), pins, None)
        return lib_id

    def add_custom(self, lib_id: str, block: str, pins: dict):
        """登记一个自定义符号（block 为完整 S-expr 文本）。"""
        self.symbols[lib_id] = LibSymbol(lib_id, block, pins, None)
        return lib_id

    # ---------- 放置元件 ----------
    def place(self, lib_id, ref, value, x, y, angle=0, mirror=None,
              hide_value=False, hide_ref=False, ref_at=None, val_at=None,
              footprint="", extra_props=(), ref_justify=None,
              in_bom=True, on_board=True):
        assert lib_id in self.symbols, f"符号 {lib_id} 未加载"
        uu = uid()
        c = Component(self, lib_id, ref, value, x, y, angle, mirror, uu)
        self.items.append(("component", c, dict(
            hide_value=hide_value, hide_ref=hide_ref, ref_at=ref_at, val_at=val_at,
            footprint=footprint, extra_props=extra_props, ref_justify=ref_justify,
            in_bom=in_bom, on_board=on_board)))
        return c

    # ---------- 图元 ----------
    def wire(self, p1, p2, width=0):
        self.items.append(("wire", [p1, p2], width))

    def wire_path(self, pts, width=0):
        """折线：pts 为点列表 [(x1,y1),(x2,y2),...]。

        注意：KiCad 的 (wire) 只接受两个端点，多段折线拆成多条 wire。
        """
        pts = list(pts)
        for i in range(len(pts) - 1):
            if pts[i] != pts[i + 1]:
                self.items.append(("wire", [pts[i], pts[i + 1]], width))

    def junction(self, x, y):
        self.items.append(("junction", (x, y), None))

    def label(self, text, x, y, angle=0, justify="left bottom"):
        self.items.append(("label", text, (x, y, angle, justify)))

    def glabel(self, text, x, y, angle=0, shape="input", justify=None):
        """全局标签（跨页同名相连）。"""
        self.items.append(("glabel", text, (x, y, angle, shape, justify)))

    def text(self, s, x, y, size=1.27, justify="left bottom", color=None):
        self.items.append(("text", s, (x, y, size, justify, color)))

    def no_connect(self, x, y):
        self.items.append(("noconnect", (x, y), None))

    def power(self, kind, x, y, angle=0, ref=None):
        """放置电源符号：kind ∈ {GND, +3V3, +5V, ...}。

        Reference(#PWRxx) 隐藏；Value(GND/3V3 文字) 显示在符号外侧。
        """
        lib_id = self.load("power", kind)
        n = sum(1 for t, a, b in self.items if t == "component" and a.lib_id.startswith("power:"))
        r = ref or f"#PWR{n + 1:02d}"
        val_dy = 6.35 if kind == "GND" else -4.5
        return self.place(lib_id, r, kind, x, y, angle, hide_ref=True,
                          val_at=(x, y + val_dy, 0))

    # ---------- 连线辅助 ----------
    @staticmethod
    def route_hv(p1, p2, first="h"):
        """返回 L 型折线的中间点列表（先水平后垂直 / 先垂直后水平）。"""
        (x1, y1), (x2, y2) = p1, p2
        if first == "h":
            return [(x1, y1), (x2, y1), (x2, y2)]
        return [(x1, y1), (x1, y2), (x2, y2)]

    def connect(self, pa, pb, first="h", both_wire=True):
        """用 L 型线连接两点。返回中间拐点（便于放 junction）。"""
        path = self.route_hv(pa, pb, first)
        if path[1] != path[2]:
            self.wire_path(path)
        return path

    def tap(self, p_from, p_node):
        """从主线上的点接出一段线到节点。"""
        self.wire_path([p_from, p_node])
        if p_from != p_node:
            self.junction(*p_from)

    # ---------- 输出 ----------
    def _emit_component(self, c: Component, opt: dict):
        L = []
        mirror = f'\n\t\t(mirror {c.mirror})' if c.mirror else ""
        L.append("\t(symbol")
        L.append(f'\t\t(lib_id "{c.lib_id}")')
        L.append(f"\t\t(at {fmt(c.x)} {fmt(c.y)} {c.angle}){mirror}")
        L.append("\t\t(unit 1)")
        L.append("\t\t(exclude_from_sim no)")
        L.append(f'\t\t(in_bom {"yes" if opt.get("in_bom", True) else "no"})')
        L.append(f'\t\t(on_board {"yes" if opt.get("on_board", True) else "no"})')
        L.append("\t\t(dnp no)")
        L.append(f'\t\t(uuid "{c.uuid}")')
        props = [("Reference", c.ref, opt.get("ref_at"), opt.get("ref_justify")),
                 ("Value", c.value, opt.get("val_at"), None)]
        if opt.get("footprint"):
            props.append(("Footprint", opt["footprint"], None, None))
        props.extend(opt.get("extra_props", ()))
        for name, val, at, justify in props:
            if at is None:
                at = (c.x, c.y, 0)
            hide = ""
            if name == "Value" and opt.get("hide_value"):
                hide = "\n\t\t\t\t(hide yes)"
            if name == "Reference" and opt.get("hide_ref"):
                hide = "\n\t\t\t\t(hide yes)"
            if name in ("Footprint", "Datasheet"):
                hide = "\n\t\t\t\t(hide yes)"
            j = ""
            if justify:
                j = f"\n\t\t\t\t(justify {justify})"
            L.append(f'\t\t(property "{name}" "{val}"')
            L.append(f"\t\t\t(at {fmt(at[0])} {fmt(at[1])} {at[2] if len(at) > 2 else 0})")
            L.append("\t\t\t(effects")
            L.append("\t\t\t\t(font")
            L.append("\t\t\t\t\t(size 1.27 1.27)")
            L.append("\t\t\t\t)" + hide + j)
            L.append("\t\t\t)")
            L.append("\t\t)")
        sym = self.symbols[c.lib_id]
        for num in sym.pins:
            L.append(f'\t\t(pin "{num}"')
            L.append(f'\t\t\t(uuid "{uid()}")')
            L.append("\t\t)")
        L.append("\t\t(instances")
        L.append(f'\t\t\t(project "{self.project}"')
        L.append(f'\t\t\t\t(path "/{self.uuid}"')
        L.append(f'\t\t\t\t\t(reference "{c.ref}")')
        L.append("\t\t\t\t\t(unit 1)")
        L.append("\t\t\t\t)")
        L.append("\t\t\t)")
        L.append("\t\t)")
        L.append("\t)")
        return "\n".join(L)

    def _emit_item(self, item):
        kind, a, b = item
        if kind == "component":
            return self._emit_component(a, b)
        if kind == "wire":
            L = ["\t(wire", "\t\t(pts"]
            for (x, y) in a:
                L.append(f"\t\t\t(xy {fmt(x)} {fmt(y)})")
            L.append("\t\t)")
            L.append("\t\t(stroke")
            L.append(f"\t\t\t(width {fmt(b)})")
            L.append("\t\t\t(type default)")
            L.append("\t\t)")
            L.append(f'\t\t(uuid "{uid()}")')
            L.append("\t)")
            return "\n".join(L)
        if kind == "junction":
            x, y = a
            L = ["\t(junction", f"\t\t(at {fmt(x)} {fmt(y)})",
                 "\t\t(diameter 0)", "\t\t(color 0 0 0 0)",
                 f'\t\t(uuid "{uid()}")', "\t)"]
            return "\n".join(L)
        if kind == "label":
            text, (x, y, angle, justify) = a, b
            L = [f'\t(label "{text}"', f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
                 "\t\t(effects", "\t\t\t(font", "\t\t\t\t(size 1.27 1.27)", "\t\t\t)",
                 f"\t\t\t(justify {justify})", "\t\t)", f'\t\t(uuid "{uid()}")', "\t)"]
            return "\n".join(L)
        if kind == "glabel":
            text, (x, y, angle, shape, justify) = a, b
            j = justify if justify else ("right" if angle == 180 else "left")
            L = [f'\t(global_label "{text}"', f"\t\t(shape {shape})",
                 f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
                 "\t\t(effects", "\t\t\t(font", "\t\t\t\t(size 1.27 1.27)", "\t\t\t)",
                 f"\t\t\t(justify {j})", "\t\t)",
                 "\t\t(fields_autoplaced yes)",
                 '\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"',
                 f"\t\t\t(at {fmt(x)} {fmt(y)} 0)",
                 "\t\t\t(effects", "\t\t\t\t(font", "\t\t\t\t\t(size 1.27 1.27)", "\t\t\t\t)",
                 "\t\t\t\t(hide yes)", "\t\t\t)", "\t\t)",
                 f'\t\t(uuid "{uid()}")', "\t)"]
            return "\n".join(L)
        if kind == "text":
            s, (x, y, size, justify, color) = a, b
            col = ""
            L = [f'\t(text "{s}"', f"\t\t(exclude_from_sim no)",
                 f"\t\t(at {fmt(x)} {fmt(y)} 0)",
                 "\t\t(effects", "\t\t\t(font", f"\t\t\t\t(size {fmt(size)} {fmt(size)})",
                 "\t\t\t)", f"\t\t\t(justify {justify}){col}", "\t\t)",
                 f'\t\t(uuid "{uid()}")', "\t)"]
            return "\n".join(L)
        if kind == "noconnect":
            x, y = a
            return "\n".join(["\t(no_connect", f"\t\t(at {fmt(x)} {fmt(y)})",
                              f'\t\t(uuid "{uid()}")', "\t)"])
        raise ValueError(kind)

    def dumps(self) -> str:
        L = ["(kicad_sch",
             "\t(version 20251024)",
             '\t(generator "eeschema")',
             '\t(generator_version "10.0")',
             f'\t(uuid "{self.uuid}")',
             f'\t(paper "{self.paper}")']
        if self.title or self.date or self.rev or self.company or self.comments:
            L.append("\t(title_block")
            if self.title:
                L.append(f'\t\t(title "{self.title}")')
            if self.date:
                L.append(f'\t\t(date "{self.date}")')
            if self.rev:
                L.append(f'\t\t(rev "{self.rev}")')
            if self.company:
                L.append(f'\t\t(company "{self.company}")')
            for i, cm in enumerate(self.comments, 1):
                L.append(f'\t\t(comment {i} "{cm}")')
            L.append("\t)")
        L.append("\t(lib_symbols")
        for s in self.symbols.values():
            L.append(indent(s.block, 2))
        L.append("\t)")
        for item in self.items:
            L.append(self._emit_item(item))
        L.append("\t(sheet_instances")
        L.append('\t\t(path "/"')
        L.append('\t\t\t(page "1")')
        L.append("\t\t)")
        L.append("\t)")
        L.append('\t(embedded_fonts no)')
        L.append(")")
        return "\n".join(L) + "\n"

    def save(self, path: str):
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.dumps())
        return path


def module_symbol(lib_id: str, ref_prefix: str, value: str, pins: list,
                  w: float, h: float, desc: str = "", pin_len: float = 2.54,
                  bg: bool = True):
    """生成一个"模块框"自定义符号。

    pins: [(引脚名, 引脚号, 方位, 位置偏移, 类型)]，方位 ∈ {'l','r','t','b'}
          'l'/'r' 的位置偏移是 y（库坐标，向上为正）；
          't'/'b' 的位置偏移是 x。
    返回 (S-expr 文本, {引脚号: (x, y, angle)})
    """
    name = lib_id.split(":")[-1]
    pin_txt = []
    pin_map = {}
    for pname, pnum, side, off, ptype in pins:
        if side == "l":
            at, rot = (-w - pin_len, off, 0), 0
        elif side == "r":
            at, rot = (w + pin_len, off, 180), 180
        elif side == "t":
            at, rot = (off, h + pin_len, 270), 270
        else:  # b
            at, rot = (off, -h - pin_len, 90), 90
        pin_map[str(pnum)] = (at[0], at[1], at[2])
        pin_txt.append(f"""\t\t(pin {ptype} line
\t\t\t(at {fmt(at[0])} {fmt(at[1])} {rot})
\t\t\t(length {fmt(pin_len)})
\t\t\t(name "{pname}"
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t)
\t\t\t(number "{pnum}"
\t\t\t\t(effects
\t\t\t\t\t(font
\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t)
\t\t)""")
    fill = "background" if bg else "none"
    txt = f"""\t(symbol "{lib_id}"
\t\t(pin_names
\t\t\t(offset 0.762)
\t\t)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(property "Reference" "{ref_prefix}"
\t\t\t(at 0 {fmt(h + 3.81)} 0)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(property "Value" "{value}"
\t\t\t(at 0 {fmt(-h - 3.81)} 0)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(property "Footprint" ""
\t\t\t(at 0 0 0)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t\t(hide yes)
\t\t\t)
\t\t)
\t\t(property "Datasheet" ""
\t\t\t(at 0 0 0)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t\t(hide yes)
\t\t\t)
\t\t)
\t\t(property "Description" "{desc}"
\t\t\t(at 0 0 0)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t\t(hide yes)
\t\t\t)
\t\t)
\t\t(symbol "{name}_0_1"
\t\t\t(rectangle
\t\t\t\t(start {fmt(-w)} {fmt(h)})
\t\t\t\t(end {fmt(w)} {fmt(-h)})
\t\t\t\t(stroke
\t\t\t\t\t(width 0.254)
\t\t\t\t\t(type default)
\t\t\t\t)
\t\t\t\t(fill
\t\t\t\t\t(type {fill})
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(symbol "{name}_1_1"
{chr(10).join(pin_txt)}
\t\t)
\t)"""
    return txt, pin_map
