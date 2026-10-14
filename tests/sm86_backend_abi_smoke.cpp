#include "../OptiScaler/framegen/dlssg/Sm86BackendAbi.h"

#include <cassert>
#include <cstdio>

int main()
{
    using namespace AmpereMfgLoader::BackendAbi;
    Info info;
    Install install;
    PluginConfig plugin;
    ImageApproxConfig image;
    assert(info.size == sizeof(info) && info.version == 1);
    assert(install.size == sizeof(install) && install.version == 1);
    assert(install.config.size == sizeof(install.config) && install.config.version == 1);
    assert(install.config.maxGeneratedFrames == 5 && plugin.maxGeneratedFrames == 5);
    assert(install.flags == 0x0018004d);
    assert(install.config.logLevel == 0 && install.config.logFile == 0 && install.config.debugOutput == 0);
    assert(install.runtime == nullptr && install.logPath == nullptr);
    assert(plugin.size == sizeof(plugin) && plugin.version == 1);
    assert(image.size == sizeof(image) && image.mask == 0 && image.overrideVariant == 0);
    std::puts("sm86_backend_abi_smoke: versioned layouts and native backend defaults verified");
}
