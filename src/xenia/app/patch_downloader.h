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

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
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

  // Download all patches (fetches list and downloads in parallel batches)
  void DownloadAllPatches(const std::filesystem::path& destination_dir,
                          const std::unordered_map<std::string, size_t>& existing_files,
                          ProgressCallback progress_callback,
                          CompletionCallback completion_callback);
  
  // Cancel any ongoing download
  void CancelDownload();

 private:
  static constexpr const char* kGitHubApiUrl =
      "https://api.github.com/repos/xenia-canary/game-patches/contents/patches";

  // HTTP request implementation
  bool HttpGet(const std::string& url, std::string& response);
  
  // Merge downloaded patches with existing file
  bool MergePatchFile(const std::filesystem::path& existing_path,
                     const std::string& new_content);
  
  std::future<void> download_task_;
  std::atomic<bool> cancel_requested_{false};
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_PATCH_DOWNLOADER_H_