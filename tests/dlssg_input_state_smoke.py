"""Run the production game's DLSSG state adapter with fake output lifecycles.

The extracted functions are compiled against the real Streamline SDK structures.
No Windows DLL or GPU is required. This covers the pre-swapchain menu query that
previously cached a 2X-only limit, and compatibility with old structure versions.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "OptiScaler/hooks/Streamline_Hooks.cpp").read_text(encoding="utf-8")
config_source = (ROOT / "OptiScaler/Config.h").read_text(encoding="utf-8-sig")
optional_storage = config_source[config_source.index("enum HasDefaultValue"):config_source.index("constexpr inline int UnboundKey")]


def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


preamble = r'''
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <sl_dlss_g.h>
'''+ optional_storage + r'''
enum class FGOutput { NoFG, DLSSG, XeFG };
struct FakeOutput {
    int maximum = 3;
    int generated = 2;
    bool active = true;
    bool paused = false;
    bool dynamic = true;
    int GetMaxInterpolationCount() { return maximum; }
    int GetInterpolatedFrameCount() { return generated; }
    bool IsActive() { return active; }
    bool IsPaused() { return paused; }
    bool GetDMFGSupport() { return dynamic; }
};
struct State {
    FGOutput activeFgOutput = FGOutput::DLSSG;
    std::optional<unsigned> dlssgMfgMax;
    void* currentD3D12Device = nullptr;
    FakeOutput* output = nullptr;
    FakeOutput* CurrentFG() { return output; }
    static State& Instance() { static State value; return value; }
};
namespace AmpereMfgLoader {
struct Status { bool BackendReady = false; unsigned MaxGeneratedFrames = 5; };
static Status backend;
static Status LastStatus() { return backend; }
}
struct XeFG_Dx12 { static int QueryMaxInterpolationCount(void*) { return 3; } };
'''

checks = r'''
int main() {
    // The real storage must persist explicit None while leaving auto detection
    // volatile, so saving and restarting does not undo the user's opt-out.
    CustomOptional<FGOutput, SoftDefault> selection(FGOutput::NoFG);
    assert(!selection.has_value() && !selection.value_for_config().has_value());
    selection.set_volatile_value(FGOutput::DLSSG);
    assert(selection == FGOutput::DLSSG && !selection.value_for_config().has_value());
    selection = FGOutput::NoFG;
    assert(selection.value_for_config() == FGOutput::NoFG);
    CustomOptional<FGOutput, SoftDefault> reloaded(FGOutput::NoFG);
    reloaded.set_from_config(selection.value_for_config());
    assert(reloaded.has_value() && reloaded == FGOutput::NoFG);

    auto& app = State::Instance();
    sl::DLSSGState state;
    AmpereMfgLoader::backend.BackendReady = true;
    FillDlssgInputState(state);
    assert(state.numFramesToGenerateMax == 5); // Menu query before creating any swapchain.
    assert(state.numFramesActuallyPresented == 1);
    assert(state.bIsDynamicMFGSupported == sl::Boolean::eFalse); // No invented live support.

    app.dlssgMfgMax = 2;
    FillDlssgInputState(state);
    assert(state.numFramesToGenerateMax == 2); // Actual plugin limit outranks the backend ceiling.
    FakeOutput output;
    app.output = &output;
    FillDlssgInputState(state);
    assert(state.numFramesToGenerateMax == 3 && state.numFramesActuallyPresented == 3);
    assert(state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
    output.paused = true;
    FillDlssgInputState(state);
    assert(state.numFramesActuallyPresented == 1);

    // Guard fields outside old SDK versions, even when presented with a larger allocation.
    state.structVersion = sl::kStructVersion1;
    state.numFramesToGenerateMax = 99;
    state.bIsDynamicMFGSupported = sl::Boolean::eFalse;
    FillDlssgInputState(state);
    assert(state.numFramesToGenerateMax == 99 && state.bIsDynamicMFGSupported == sl::Boolean::eFalse);
    state.structVersion = sl::kStructVersion2;
    FillDlssgInputState(state);
    assert(state.numFramesToGenerateMax == 3 && state.bIsDynamicMFGSupported == sl::Boolean::eFalse);

    app.output = nullptr;
    app.dlssgMfgMax.reset();
    AmpereMfgLoader::backend.BackendReady = false;
    assert(OutputMaxInterpolationCount() == 1);
    AmpereMfgLoader::backend.BackendReady = true;
    app.activeFgOutput = FGOutput::NoFG;
    assert(OutputMaxInterpolationCount() == 1); // Disabled output must not advertise SM86.
    std::puts("Production DLSSG input state: early MFG menu, live limits and SDK versions passed.");
}
'''

with tempfile.TemporaryDirectory(prefix="optiscaler-dlssg-input-") as directory:
    fixture = Path(directory)
    cpp = fixture / "state.cpp"
    cpp.write_text(preamble + function("static int OutputMaxInterpolationCount()") + "\n" +
                   function("static void FillDlssgInputState(") + checks, encoding="utf-8")
    includes = str(ROOT / "external/streamline")
    if os.name == "nt":
        exe = fixture / "state.exe"
        command = ["cl", "/nologo", "/std:c++20", "/EHsc", "/W4", f"/I{includes}", str(cpp), f"/Fe:{exe}"]
    else:
        exe = fixture / "state"
        command = [shutil.which("c++") or "g++", "-std=c++20", f"-I{includes}", str(cpp), "-o", str(exe)]
    subprocess.run(command, cwd=fixture, check=True)
    subprocess.run([str(exe)], check=True)
