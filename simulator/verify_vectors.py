import sys, struct

def build_frame(sensors):
    data = bytes([len(sensors)])
    for sid, v in sensors:
        data += bytes([sid]) + struct.pack(">h", v)
    body = bytes([0x01, len(data)]) + data
    return b"\xAA\x55" + body + bytes([sum(body) & 0xFF])

def hx(b): return " ".join(f"{x:02X}" for x in b)

VECTORS = {
    "v1": bytes.fromhex("AA 55 01 0D 04 01 01 00 02 02 26 03 01 38 04 01 64 E3"),
    "v2": bytes.fromhex("AA 55 01 07 02 01 00 C8 02 01 C2 98"),
    "v3": bytes.fromhex("AA 55 01 07 02 01 FF CC 02 02 58 32"),
}

cases = {
    "v1": [(1, 256), (2, 550), (3, 312), (4, 356)],
    "v2": [(1, 200), (2, 450)],
    "v3": [(1, -52), (2, 600)],
}
ok = True
for name, sensors in cases.items():
    got, want = build_frame(sensors), VECTORS[name]
    match = got == want
    ok &= match
    print(f"{name}: {'PASS' if match else 'FAIL'}  gen={hx(got)}")
    if not match:
        print(f"        expect={hx(want)}")
print("ALL PASS" if ok else "MISMATCH FOUND")
