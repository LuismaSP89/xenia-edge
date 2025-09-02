/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/patch_downloader.h"

#include <curl/curl.h>
#include <fstream>
#include <thread>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "xenia/base/logging.h"

namespace xe {
namespace app {

// Callback for CURL to write data to string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb,
                            std::string* userp) {
  size_t total_size = size * nmemb;
  userp->append((char*)contents, total_size);
  return total_size;
}

// Callback for CURL to write data to file
static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb,
                                std::ofstream* file) {
  size_t total_size = size * nmemb;
  file->write((char*)contents, total_size);
  return total_size;
}

// Progress callback for CURL
struct ProgressData {
  PatchDownloader::ProgressCallback callback;
  size_t last_reported = 0;
};

static int XferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
  auto* progress = static_cast<ProgressData*>(clientp);
  if (progress->callback && dltotal > 0) {
    // Only report every 1% to avoid too many callbacks
    size_t percent = (dlnow * 100) / dltotal;
    if (percent != progress->last_reported) {
      progress->last_reported = percent;
      progress->callback(dlnow, dltotal);
    }
  }
  return 0;
}

PatchDownloader::PatchDownloader() {
  // Initialize CURL globally (could be done elsewhere)
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

PatchDownloader::~PatchDownloader() {
  // Cleanup is handled by application shutdown
}

void PatchDownloader::FetchAvailablePatches(
    std::function<void(const std::vector<PatchFileInfo>&)> callback) {
  std::thread([this, callback]() {
    std::vector<PatchFileInfo> patches;
    std::string response;

    if (!HttpGet(kGitHubApiUrl, response)) {
      XELOGE("Failed to fetch patch list from GitHub");
      callback(patches);
      return;
    }

    // Parse JSON response
    rapidjson::Document doc;
    doc.Parse(response.c_str());

    if (doc.HasParseError()) {
      XELOGE("Failed to parse GitHub API response: {}",
             rapidjson::GetParseError_En(doc.GetParseError()));
      callback(patches);
      return;
    }

    if (!doc.IsArray()) {
      XELOGE("Unexpected GitHub API response format");
      callback(patches);
      return;
    }

    for (const auto& item : doc.GetArray()) {
      if (!item.IsObject()) continue;

      PatchFileInfo info;

      if (item.HasMember("name") && item["name"].IsString()) {
        info.filename = item["name"].GetString();

        // Only include .patch.toml files
        if (info.filename.find(".patch.toml") == std::string::npos) {
          continue;
        }
      }

      if (item.HasMember("download_url") && item["download_url"].IsString()) {
        info.download_url = item["download_url"].GetString();
      }

      if (item.HasMember("sha") && item["sha"].IsString()) {
        info.sha = item["sha"].GetString();
      }

      if (item.HasMember("size") && item["size"].IsUint64()) {
        info.size = item["size"].GetUint64();
      }

      if (!info.filename.empty() && !info.download_url.empty()) {
        patches.push_back(std::move(info));
      }
    }

    XELOGI("Found {} patch files available for download", patches.size());
    callback(patches);
  }).detach();
}

void PatchDownloader::DownloadPatch(const PatchFileInfo& patch_info,
                                    const std::filesystem::path& destination,
                                    ProgressCallback progress_callback,
                                    CompletionCallback completion_callback) {
  std::thread([this, patch_info, destination, progress_callback,
               completion_callback]() {
    bool success = HttpDownloadFile(patch_info.download_url, destination,
                                    progress_callback);

    if (completion_callback) {
      if (success) {
        completion_callback(true, "");
      } else {
        completion_callback(false, "Download failed");
      }
    }
  }).detach();
}

void PatchDownloader::DownloadAllPatches(
    const std::filesystem::path& destination_dir,
    ProgressCallback progress_callback,
    CompletionCallback completion_callback) {
  FetchAvailablePatches([this, destination_dir, progress_callback,
                         completion_callback](
                            const std::vector<PatchFileInfo>& patches) {
    if (patches.empty()) {
      if (completion_callback) {
        completion_callback(false, "No patches available");
      }
      return;
    }

    std::thread([this, patches, destination_dir, progress_callback,
                 completion_callback]() {
      size_t total_patches = patches.size();
      size_t completed = 0;

      for (const auto& patch : patches) {
        auto dest_path = destination_dir / patch.filename;

        // Skip if file already exists and has the same size
        if (std::filesystem::exists(dest_path)) {
          auto existing_size = std::filesystem::file_size(dest_path);
          if (existing_size == patch.size) {
            completed++;
            if (progress_callback) {
              progress_callback(completed, total_patches);
            }
            continue;
          }
        }

        bool success = HttpDownloadFile(patch.download_url, dest_path, nullptr);

        if (!success) {
          XELOGE("Failed to download: {}", patch.filename);
        }

        completed++;
        if (progress_callback) {
          progress_callback(completed, total_patches);
        }
      }

      if (completion_callback) {
        completion_callback(true, "");
      }
    }).detach();
  });
}

bool PatchDownloader::HttpGet(const std::string& url, std::string& response) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return false;
  }

  response.clear();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xenia-Canary");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_easy_cleanup(curl);

  return res == CURLE_OK && http_code == 200;
}

bool PatchDownloader::HttpDownloadFile(const std::string& url,
                                       const std::filesystem::path& destination,
                                       ProgressCallback progress_callback) {
  // Create parent directory if it doesn't exist
  std::filesystem::create_directories(destination.parent_path());

  std::ofstream file(destination, std::ios::binary);
  if (!file.is_open()) {
    XELOGE("Failed to open file for writing: {}", destination.string());
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    return false;
  }

  ProgressData progress_data{progress_callback, 0};

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xenia-Canary");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 minutes timeout

  if (progress_callback) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_easy_cleanup(curl);
  file.close();

  bool success = res == CURLE_OK && http_code == 200;

  if (!success) {
    // Remove partial download
    std::filesystem::remove(destination);
  }

  return success;
}

}  // namespace app
}  // namespace xe
