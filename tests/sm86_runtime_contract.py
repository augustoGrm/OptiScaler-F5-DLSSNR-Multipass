"""Validate the exact binary/control ABI used by the embedded SM86 adapter.

This is an offline PE check, not a substitute for a Windows/GPU integration test.
Run after get_sm86_runtime.ps1: python tests/sm86_runtime_contract.py.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "redist/sm86/manifest.json").read_text())
for entry in manifest["files"]:
    data = (ROOT / "redist/sm86" / entry["path"]).read_bytes()
    assert hashlib.sha256(data).hexdigest() == entry["sha256"], entry["path"]

data = (ROOT / "redist/sm86/version.dll").read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe:pe + 4] == b"PE\0\0"
machine, sections = struct.unpack_from("<HH", data, pe + 4)
assert machine == 0x8664, "The adapter requires the pinned x64 image"
optional_size = struct.unpack_from("<H", data, pe + 20)[0]
optional = pe + 24
assert struct.unpack_from("<H", data, optional)[0] == 0x20B
section_table = optional + optional_size


def offset(rva):
    for i in range(sections):
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from(
            "<IIII", data, section_table + i * 40 + 8)
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_address + rva - virtual_address
    raise AssertionError(f"Unmapped RVA: {rva:x}")


export_rva = struct.unpack_from("<I", data, optional + 112)[0]
directory = offset(export_rva)
count = struct.unpack_from("<I", data, directory + 24)[0]
function_rva, name_rva, ordinal_rva = struct.unpack_from("<III", data, directory + 28)
exports = {}
for i in range(count):
    name = offset(struct.unpack_from("<I", data, offset(name_rva) + i * 4)[0])
    end = data.index(b"\0", name)
    ordinal = struct.unpack_from("<H", data, offset(ordinal_rva) + i * 2)[0]
    function = struct.unpack_from("<I", data, offset(function_rva) + ordinal * 4)[0]
    exports[data[name:end].decode("ascii")] = offset(function)

for name, expected_size in [("DLSSG_RequestControl", 20), ("DLSSG_GetControlStatus", 92)]:
    code = data[exports[name]:exports[name] + 256]
    assert b"\x83\x39" + bytes([expected_size]) in code, f"{name}: unexpected struct size"
    assert b"\x83\x79\x04\x01" in code, f"{name}: unexpected ABI version"
assert "DLSSG_UniversalProxy" in exports
print("sm86_runtime_contract: payload, licences, x64 exports and v1 struct sizes verified")
