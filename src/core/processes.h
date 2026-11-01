// Finding and closing the running programs of an installed app, so its
// installer can replace the files.
#pragma once

#include <filesystem>

namespace vitool {

// Whether any process runs an executable from inside the folder.
bool IsRunningFrom(const std::filesystem::path& folder);

// Asks those processes to close (WM_CLOSE to their windows) and ends the ones
// still running after a few seconds. Returns false if some could not be ended.
bool CloseProgramsIn(const std::filesystem::path& folder);

}  // namespace vitool
