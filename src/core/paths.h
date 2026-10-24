#pragma once

#include <filesystem>

namespace vitool {

std::filesystem::path ExecutableDir();
// %LOCALAPPDATA%\Vitool Box, created on first use.
std::filesystem::path DataDir();

}  // namespace vitool
