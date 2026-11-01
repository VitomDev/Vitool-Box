#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vitool {

// Called with the bytes received so far and the total (0 when unknown).
// Returning false cancels the download.
using DownloadProgress = std::function<bool(uint64_t received, uint64_t total)>;

// Downloads an HTTPS URL to a file, following redirects. Returns an empty
// string on success, otherwise an error key for the UI ("cancelled",
// "network", "http|404"...).
std::wstring DownloadFile(const std::wstring& url, const std::filesystem::path& target, const DownloadProgress& progress);

// Lowercase hex SHA-256 of a file, or an empty string if it cannot be read.
std::string Sha256File(const std::filesystem::path& file);

}  // namespace vitool
