"""Check the exact embedded NR inputs without loading or executing either DLL.

Run after get_nr_runtime.ps1: python tests/nr_runtime_contract.py.
"""
import argparse
import hashlib
import json
import mmap
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PeImage:
    def __init__(self, data):
        self.data = data
        assert data[:2] == b"MZ", "Missing DOS header"
        pe = self.u32(0x3C)
        assert data[pe:pe + 4] == b"PE\0\0", "Missing PE header"
        assert self.u16(pe + 4) == 0x8664, "NR requires an x64 DLL"
        assert self.u16(pe + 22) & 0x2000, "Input must be a DLL"
        self.optional = pe + 24
        assert self.u16(self.optional) == 0x20B, "NR requires PE32+"
        section_table = self.optional + self.u16(pe + 20)
        self.sections = [struct.unpack_from("<4I", data, section_table + i * 40 + 8)
                         for i in range(self.u16(pe + 6))]

    def u16(self, offset):
        return struct.unpack_from("<H", self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from("<I", self.data, offset)[0]

    def offset(self, rva):
        for virtual_size, address, raw_size, raw in self.sections:
            if address <= rva < address + max(virtual_size, raw_size):
                result = raw + rva - address
                assert 0 <= result < len(self.data), "PE RVA exceeds the file"
                return result
        raise AssertionError(f"Unmapped PE RVA: {rva:x}")

    def exports(self):
        directory = self.offset(self.u32(self.optional + 112))
        count = self.u32(directory + 24)
        table = self.offset(self.u32(directory + 32))
        names = set()
        for i in range(count):
            begin = self.offset(self.u32(table + i * 4))
            end = self.data.find(b"\0", begin)
            assert end > begin, "Unterminated export name"
            names.add(self.data[begin:end].decode("ascii"))
        return names

    def file_version(self):
        resources = self.u32(self.optional + 128)
        if not resources:
            return None
        base = self.offset(resources)

        def entries(relative):
            directory = base + relative
            count = self.u16(directory + 12) + self.u16(directory + 14)
            return [struct.unpack_from("<II", self.data, directory + 16 + i * 8)
                    for i in range(count)]

        versions = [target for key, target in entries(0) if key == 16]
        if not versions:
            return None
        target = versions[0]
        while target & 0x80000000:
            target = entries(target & 0x7FFFFFFF)[0][1]
        rva, size = struct.unpack_from("<II", self.data, base + target)
        start = self.offset(rva)
        signature = self.data.find(b"\xbd\x04\xef\xfe", start, start + size)
        assert signature != -1, "Missing VS_FIXEDFILEINFO"
        major, minor = struct.unpack_from("<II", self.data, signature + 8)
        return (major >> 16, major & 65535, minor >> 16, minor & 65535)


def check_model_parts(directory, entry):
    parts = entry["parts"]
    assert len(parts) == 6, "The exact model must be stored in six build-input parts"
    combined = hashlib.sha256()
    total = 0
    for index, part in enumerate(parts, 1):
        assert part["path"] == f"model/part{index:03d}", "Unexpected model part order/path"
        assert 0 < part["size"] <= 30 * 1024 * 1024, "Oversized model part"
        if index < 6:
            assert part["size"] == 30 * 1024 * 1024, "Unexpected model part boundary"
        path = directory / part["path"]
        assert path.stat().st_size == part["size"], f"{part['path']}: size mismatch"
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
                combined.update(block)
                total += len(block)
        assert digest.hexdigest() == part["sha256"], f"{part['path']}: hash mismatch"
    assert total == entry["size"], "Model parts do not reconstruct the supplied file size"
    assert combined.hexdigest() == entry["sha256"], "Model parts do not reconstruct the supplied DLL"


def check(directory):
    manifest = json.loads((ROOT / "redist/nr/manifest.json").read_text())
    expected = {
        "forwarder": ("nvngx.dll_dlssnr.dll", 152064, 31101,
                      "89766bcb38a9caea9a2e3f767923e6cda8b9b51a6a66ca14ac9473d734e12efa"),
        "model": ("nvngx_dlssnr.dll", 165840496, 31102,
                  "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"),
    }
    required = {
        "forwarder": {"dlssnr_call_create", "dlssnr_call_evaluate_v2", "dlssnr_call_release",
                      "dlssnr_call_set_extras", "dlssnr_call_probe_float", "dlssnr_call_set_float_slot",
                      "dlssnr_vk_probe", "dlssnr_vk_init", "dlssnr_vk_create", "dlssnr_vk_evaluate_v2",
                      "dlssnr_vk_release", "dlssnr_vk_shutdown", "dlssnr_vk_forget"},
        "model": {f"NVSDK_NGX_{api}_{entry}" for api in ("D3D12", "VULKAN")
                  for entry in ("Init_Ext", "CreateFeature", "EvaluateFeature", "ReleaseFeature")},
    }
    assert {entry["role"] for entry in manifest["files"]} == set(expected)
    for entry in manifest["files"]:
        role = entry["role"]
        name, size, resource_id, sha256 = expected[role]
        assert (entry["path"], entry["size"], entry["resource_id"], entry["sha256"]) == (
            name, size, resource_id, sha256), role
        if role == "model":
            check_model_parts(directory, entry)
        path = directory / name
        assert path.stat().st_size == size, f"{name}: run get_nr_runtime.ps1 to reconstruct the pinned input"
        with path.open("rb") as stream:
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            assert digest.hexdigest() == sha256, f"{name}: hash mismatch"
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                pe = PeImage(data)
                assert required[role] <= pe.exports(), f"{name}: missing required exports"
                if role == "model":
                    assert pe.file_version() == (310, 8, 0, 0), "Unexpected NR model version"

    bundle_id = hashlib.sha256("".join(entry["sha256"] for entry in manifest["files"]).encode("ascii")).hexdigest()
    assert manifest["bundle_id"] == bundle_id, "The pair's cache identity must match its exact inputs"
    loader = (ROOT / "OptiScaler/dlssnr/DlssNrBundle.cpp").read_text()
    assert bundle_id in loader
    assert all(values[3] in loader for values in expected.values()), "Loader hashes differ from the pinned inputs"
    print("nr_runtime_contract: six exact model parts, supplied DLL pair, x64 exports and NR 310.8 verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / "redist/nr")
    check(parser.parse_args().directory)
