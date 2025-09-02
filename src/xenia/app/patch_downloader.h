/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_PATCH_DOWNLOADER_H_
#define XENIA_APP_PATCH_DOWNLOADER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xe {
namespace app {

struct PatchFileInfo {
  std::string filename;
  std::string download_url;
  std::string sha;
  size_t size;
};

class PatchDownloader {
 public:
  using ProgressCallback = std::function<void(size_t current, size_t total)>;
  using CompletionCallback =
      std::function<void(bool success, const std::string& error)>;

  PatchDownloader();
  ~PatchDownloader();

  // Fetch list of available patches from GitHub
  void FetchAvailablePatches(
      std::function<void(const std::vector<PatchFileInfo>&)> callback);

  // Download a specific patch file
  void DownloadPatch(const PatchFileInfo& patch_info,
                     const std::filesystem::path& destination,
                     ProgressCallback progress_callback,
                     CompletionCallback completion_callback);

  // Download all patches
  void DownloadAllPatches(const std::filesystem::path& destination_dir,
                          ProgressCallback progress_callback,
                          CompletionCallback completion_callback);

 private:
  static constexpr const char* kGitHubApiUrl =
      "https://api.github.com/repos/xenia-canary/game-patches/contents/patches";
  static constexpr const char* kGitHubRawUrl =
      "https://raw.githubusercontent.com/xenia-canary/game-patches/main/"
      "patches/";

  // Platform-specific HTTP request implementation
  bool HttpGet(const std::string& url, std::string& response);
  bool HttpDownloadFile(const std::string& url,
                        const std::filesystem::path& destination,
                        ProgressCallback progress_callback);
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_PATCH_DOWNLOADER_H_
