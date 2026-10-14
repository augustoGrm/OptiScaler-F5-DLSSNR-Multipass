"""Verify the exact native SM86 backend, nested payloads and binary ABI offline.

This never loads or executes a PE image. Run after get_sm86_runtime.ps1.
Passing this contract does not replace a Windows/RTX integration test.
"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / "redist/sm86/manifest.json").read_text())


class PE:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[:2] == b"MZ" and data[pe:pe + 4] == b"PE\0\0"
        machine, count = struct.unpack_from("<HH", data, pe + 4)
        assert machine == 0x8664, "The adapter requires the pinned x64 image"
        self.optional = pe + 24
        assert struct.unpack_from("<H", data, self.optional)[0] == 0x20B
        self.base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        table = self.optional + struct.unpack_from("<H", data, pe + 20)[0]
        self.sections = [struct.unpack_from("<IIII", data, table + i * 40 + 8) for i in range(count)]

    def offset(self, rva):
        for _virtual_size, virtual_address, raw_size, raw_address in self.sections:
            if virtual_address <= rva < virtual_address + raw_size:
                return raw_address + rva - virtual_address
        raise AssertionError(f"Unmapped initialized RVA: {rva:x}")

    def code(self, rva, length=256):
        start = self.offset(rva)
        return self.data[start:start + length]

    def exports(self):
        export_rva = struct.unpack_from("<I", self.data, self.optional + 112)[0]
        directory = self.offset(export_rva)
        count = struct.unpack_from("<I", self.data, directory + 24)[0]
        function_rva, name_rva, ordinal_rva = struct.unpack_from("<III", self.data, directory + 28)
        result = {}
        for i in range(count):
            name = self.offset(struct.unpack_from("<I", self.data, self.offset(name_rva) + i * 4)[0])
            end = self.data.index(b"\0", name)
            ordinal = struct.unpack_from("<H", self.data, self.offset(ordinal_rva) + i * 2)[0]
            function = struct.unpack_from("<I", self.data, self.offset(function_rva) + ordinal * 4)[0]
            result[self.data[name:end].decode("ascii")] = function
        return result

    def resource(self, resource_id):
        resource_rva = struct.unpack_from("<I", self.data, self.optional + 112 + 2 * 8)[0]
        root = self.offset(resource_rva)
        directory = root
        for key in (10, resource_id):
            count = sum(struct.unpack_from("<HH", self.data, directory + 12))
            matches = []
            for i in range(count):
                name, target = struct.unpack_from("<II", self.data, directory + 16 + i * 8)
                if name == key:
                    assert target & 0x80000000
                    matches.append(root + (target & 0x7FFFFFFF))
            assert len(matches) == 1, f"Missing/ambiguous RCDATA {resource_id}"
            directory = matches[0]
        assert sum(struct.unpack_from("<HH", self.data, directory + 12)) == 1
        target = struct.unpack_from("<I", self.data, directory + 20)[0]
        assert not target & 0x80000000
        rva, size = struct.unpack_from("<II", self.data, root + target)
        offset = self.offset(rva)
        result = self.data[offset:offset + size]
        assert len(result) == size
        return result


def verify_digest(data, expected):
    assert hashlib.sha256(data).hexdigest() == expected


for entry in manifest["files"]:
    data = (ROOT / "redist/sm86" / entry["path"]).read_bytes()
    verify_digest(data, entry["sha256"])

outer = PE((ROOT / "redist/sm86/version.dll").read_bytes())
inner = PE(outer.resource(31000))
verify_digest(inner.data, "29209dd3efe144b2beae739c88c9592d4fcbcf1cf46d51d63d5be8e7a1053554")
components = {}
for entry in manifest["embedded_components"]:
    data = outer.data
    for resource_id in entry["resource_chain"]:
        data = PE(data).resource(resource_id)
    assert len(data) == entry["size"]
    verify_digest(data, entry["sha256"])
    assert (ROOT / "redist/sm86" / entry["path"]).read_bytes() == data
    components[entry["path"]] = PE(data)

backend = components["sm86_backend.dll"]
exports = backend.exports()
assert "DLSSG_UniversalProxy" not in exports and "DLSSG_RequestControl" not in exports
assert "DlssgProxy_Name" not in exports, "Only the native backend may execute"

# Check the callable wrappers, including their versioned structure guards.
for name, size in [("DlssgMod_GetInfo", 32), ("DlssgMod_Install", 80),
                   ("DlssgMod_ConfigureImageApprox", 20)]:
    code = backend.code(exports[name])
    assert b"\x83\x39" + bytes([size]) in code, f"{name}: unexpected struct size"
    assert b"\x83\x79\x04\x01" in code, f"{name}: unexpected ABI version"
install = backend.code(exports["DlssgMod_Install"])
assert b"\x83\x79\x20\x2c" in install  # RuntimeConfig.size at offset 32
assert b"\x83\x79\x24\x01" in install  # RuntimeConfig.version at offset 36
info = backend.code(exports["DlssgMod_GetInfo"])
assert b"\xc7\x41\x08\x05\x00\x00\x00" in info  # maxGeneratedFrames = 5
assert b"\x48\x89\x41\x10" in info and b"\x48\x89\x41\x18" in info

# ConfigurePlugin's exported wrapper adds its logger callback and tail-calls the
# actual implementation. It takes a 12-byte ceiling config, not a frame override.
plugin = backend.code(exports["DlssgMod_ConfigurePlugin"])
assert plugin[22] == 0xE9
plugin_rva = exports["DlssgMod_ConfigurePlugin"] + 27 + struct.unpack_from("<i", plugin, 23)[0]
plugin = backend.code(plugin_rva)
assert b"\x83\x39\x0c" in plugin and b"\x83\x79\x04\x01" in plugin
assert b"\x8b\x41\x08" in plugin

# Verify the caller-side 80/44-byte layout independently of backend validators.
constructor = inner.code(0x7340, 0x5F)
for instruction in (b"\xc7\x01\x50\x00\x00\x00", b"\xc7\x41\x04\x01\x00\x00\x00",
                    b"\xc7\x41\x20\x2c\x00\x00\x00", b"\xc7\x41\x24\x01\x00\x00\x00",
                    b"\x48\x89\x41\x08", b"\x89\x41\x10", b"\x48\x89\x41\x18"):
    assert instruction in constructor

# Install also arms the NGX core gates/load watcher. Those live in the backend,
# so no upstream proxy is needed for the architecture/driver capability gates.
call = backend.code(0x369C0, 5)
assert call[0] == 0xE8 and 0x369C5 + struct.unpack_from("<i", call, 1)[0] == 0x76590
assert b"fg_gate_hooks\0" in backend.data and b"fg_gate_hooks_deferred\0" in backend.data

runtime = components["nvngx_dlssg.dll"]
assert "NVSDK_NGX_D3D12_CreateFeature" in runtime.exports()
assert "NVSDK_NGX_VULKAN_CreateFeature" in runtime.exports()
resource_source = (ROOT / "OptiScaler/framegen/dlssg/Sm86Runtime.rc").read_text()
assert "31011 RCDATA SM86_RUNTIME_FILE" in resource_source
assert "31012 RCDATA SM86_BACKEND_FILE" in resource_source
assert "31010 RCDATA" not in resource_source
print("sm86_runtime_contract: pinned nested payloads, direct backend ABI and NGX gates verified")
