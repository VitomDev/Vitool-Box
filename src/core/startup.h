// Starting Vitool Box with Windows (hidden, in the notification area), and
// grouping the icons of the apps inside its own.
#pragma once

namespace vitool {

inline constexpr wchar_t kBackgroundArgument[] = L"--background";

bool StartsWithWindows();
// Adds or removes the HKCU Run entry. Returns false if it could not be changed.
bool SetStartWithWindows(bool enabled);

bool GroupsTrayIcons();
bool SetGroupTrayIcons(bool enabled);

}  // namespace vitool
