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
#include <iomanip>
#include <sstream>
#include <thread>
#include <mutex>

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/error/en.h"
#include "third_party/tomlplusplus/toml.hpp"
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

PatchDownloader::PatchDownloader() {
  // Initialize CURL globally (could be done elsewhere)
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

PatchDownloader::~PatchDownloader() {
  CancelDownload();
  if (download_task_.valid()) {
    download_task_.wait();
  }
}

void PatchDownloader::CancelDownload() {
  cancel_requested_ = true;
}

void PatchDownloader::DownloadAllPatches(
    const std::filesystem::path& destination_dir,
    const std::unordered_map<std::string, size_t>& existing_files,
    ProgressCallback progress_callback,
    CompletionCallback completion_callback) {
  
  // Cancel any existing download
  if (download_task_.valid()) {
    cancel_requested_ = true;
    download_task_.wait();
  }
  
  cancel_requested_ = false;
  
  // Start download in async task
  download_task_ = std::async(std::launch::async, [this, destination_dir,
                                                    existing_files,
                                                    progress_callback,
                                                    completion_callback]() {
    std::string response;
    
    // Make API call to get patch list
    if (!HttpGet(kGitHubApiUrl, response)) {
      XELOGE("Failed to fetch patch list from GitHub");
      if (completion_callback) {
        completion_callback(false, "Failed to connect to GitHub");
      }
      return;
    }
    
    // Parse JSON response
    rapidjson::Document doc;
    doc.Parse(response.c_str());
    
    if (doc.HasParseError()) {
      XELOGE("Failed to parse GitHub API response: {}",
             rapidjson::GetParseError_En(doc.GetParseError()));
      if (completion_callback) {
        completion_callback(false, "Invalid response from GitHub");
      }
      return;
    }
    
    if (!doc.IsArray()) {
      XELOGE("Unexpected GitHub API response format");
      if (completion_callback) {
        completion_callback(false, "Unexpected response format");
      }
      return;
    }
    
    // Parse all patch files from response
    std::vector<PatchFileInfo> patches;
    for (const auto& item : doc.GetArray()) {
      if (cancel_requested_) {
        if (completion_callback) {
          completion_callback(false, "Download cancelled");
        }
        return;
      }
      
      if (!item.IsObject()) continue;
      
      PatchFileInfo info;
      
      // Get filename
      if (item.HasMember("name") && item["name"].IsString()) {
        info.filename = item["name"].GetString();
        
        // Only include .patch.toml files
        if (info.filename.find(".patch.toml") == std::string::npos) {
          continue;
        }
      } else {
        continue;
      }
      
      // Get download URL
      if (item.HasMember("download_url") && item["download_url"].IsString()) {
        info.download_url = item["download_url"].GetString();
      } else {
        continue;  // Skip if no download URL
      }
      
      // Get SHA hash
      if (item.HasMember("sha") && item["sha"].IsString()) {
        info.sha = item["sha"].GetString();
      }
      
      // Get size
      if (item.HasMember("size") && item["size"].IsUint64()) {
        info.size = item["size"].GetUint64();
      }
      
      patches.push_back(std::move(info));
    }
    
    XELOGI("Found {} patch files to process", patches.size());
    
    if (patches.empty()) {
      if (completion_callback) {
        completion_callback(true, "No patches available");
      }
      return;
    }
    
    // Create patches directory if it doesn't exist
    std::filesystem::create_directories(destination_dir);
    
    // Process patches in parallel batches
    size_t processed = 0;
    size_t updated = 0;
    size_t skipped = 0;
    size_t errors = 0;
    std::mutex progress_mutex;
    
    // Process in smaller batches to avoid overwhelming the system
    const size_t batch_size = 10;
    
    for (size_t i = 0; i < patches.size(); i += batch_size) {
      if (cancel_requested_) {
        if (completion_callback) {
          completion_callback(false, "Download cancelled");
        }
        return;
      }
      
      size_t batch_end = std::min(i + batch_size, patches.size());
      std::vector<std::thread> batch_threads;
      
      for (size_t j = i; j < batch_end; ++j) {
        const auto& patch = patches[j];
        
        batch_threads.emplace_back([this, &patch, &destination_dir,
                                    &existing_files,
                                    &processed, &updated, &skipped, &errors,
                                    &progress_mutex, &patches,
                                    &progress_callback]() {
          auto dest_path = destination_dir / patch.filename;
          
          // Always download the latest version to merge
          std::string content;
          if (!HttpGet(patch.download_url, content)) {
            std::lock_guard<std::mutex> lock(progress_mutex);
            XELOGE("Failed to download: {}", patch.filename);
            errors++;
            processed++;
            if (progress_callback) {
              progress_callback(processed, patches.size());
            }
            return;
          }
          
          // Check if file exists - if so, merge patches
          auto existing_file = existing_files.find(patch.filename);
          bool file_exists = (existing_file != existing_files.end() && 
                             existing_file->second > 0);
          
          if (file_exists) {
            // Merge the downloaded content with existing file
            bool merged = MergePatchFile(dest_path, content);
            
            std::lock_guard<std::mutex> lock(progress_mutex);
            if (merged) {
              updated++;
              XELOGI("Updated {} with new patches", patch.filename);
            } else {
              skipped++;
            }
          } else {
            // New file - just save it
            std::ofstream file(dest_path, std::ios::binary);
            if (file.is_open()) {
              file.write(content.data(), content.size());
              file.close();
              
              std::lock_guard<std::mutex> lock(progress_mutex);
              updated++;
              XELOGI("Downloaded new patch file: {}", patch.filename);
            } else {
              std::lock_guard<std::mutex> lock(progress_mutex);
              XELOGE("Failed to save: {}", patch.filename);
              errors++;
            }
          }
          
          {
            std::lock_guard<std::mutex> lock(progress_mutex);
            processed++;
            if (progress_callback) {
              progress_callback(processed, patches.size());
            }
          }
        });
      }
      
      // Wait for batch to complete
      for (auto& thread : batch_threads) {
        thread.join();
      }
    }
    
    // Report completion
    std::string status;
    if (errors > 0) {
      status = fmt::format("Completed with {} errors", errors);
    } else if (updated == 0) {
      status = "All patches already up to date";
    } else {
      status = fmt::format("Successfully updated {} patch files", updated);
    }
    
    XELOGI("Download complete: {}", status);
    
    if (completion_callback) {
      completion_callback(errors == 0, status);
    }
  });
}

// Helper function to compare TOML nodes
static bool NodesEqual(const toml::node& a, const toml::node& b) {
  // Different types = not equal
  if (a.type() != b.type()) {
    return false;
  }
  
  // For simple types, we can compare values directly
  if (a.is_string() && b.is_string()) {
    return *a.as_string() == *b.as_string();
  }
  if (a.is_integer() && b.is_integer()) {
    return *a.as_integer() == *b.as_integer();
  }
  if (a.is_floating_point() && b.is_floating_point()) {
    return *a.as_floating_point() == *b.as_floating_point();
  }
  if (a.is_boolean() && b.is_boolean()) {
    return *a.as_boolean() == *b.as_boolean();
  }
  
  // For complex types (arrays, tables), convert to string for comparison
  // This is a bit hacky but works for our use case
  if (a.is_array() && b.is_array()) {
    std::stringstream ss_a, ss_b;
    ss_a << *a.as_array();
    ss_b << *b.as_array();
    return ss_a.str() == ss_b.str();
  }
  if (a.is_table() && b.is_table()) {
    std::stringstream ss_a, ss_b;
    ss_a << *a.as_table();
    ss_b << *b.as_table();
    return ss_a.str() == ss_b.str();
  }
  
  // Default: not equal
  return false;
}

bool PatchDownloader::MergePatchFile(const std::filesystem::path& existing_path,
                                     const std::string& new_content) {
  try {
    // Parse existing file
    auto existing_config = toml::parse_file(existing_path.string());
    
    // Parse new content
    std::istringstream stream(new_content);
    auto new_config = toml::parse(stream);
    
    // Track if we made any changes
    bool modified = false;
    std::vector<std::string> skipped_updates;
    
    // Update top-level fields (title_name, title_id, hash array)
    for (auto&& [key, value] : new_config) {
      std::string key_str(key.str());
      if (key_str != "patch") {
        auto existing_value = existing_config.get(key);
        if (!existing_value || !NodesEqual(*existing_value, value)) {
          existing_config.insert_or_assign(key, value);
          modified = true;
          XELOGI("Updated {} field in {}", key_str, existing_path.filename().string());
        }
      }
    }
    
    // Get or create the patch array in existing config
    auto existing_patches = existing_config["patch"].as_array();
    if (!existing_patches) {
      existing_config.insert("patch", toml::array{});
      existing_patches = existing_config["patch"].as_array();
    }
    
    // Build a map of existing patches by their name for quick lookup
    std::unordered_map<std::string, toml::table*> existing_patch_map;
    std::unordered_map<std::string, bool> patch_enabled_state;
    
    for (auto& patch : *existing_patches) {
      if (auto* table = patch.as_table()) {
        if (auto name = table->get("name")) {
          if (auto* name_str = name->as_string()) {
            std::string patch_name = name_str->get();
            existing_patch_map[patch_name] = table;
            
            // Track enabled state
            bool is_enabled = false;
            if (auto enabled = table->get("is_enabled")) {
              if (auto* enabled_bool = enabled->as_boolean()) {
                is_enabled = enabled_bool->get();
              }
            }
            patch_enabled_state[patch_name] = is_enabled;
          }
        }
      }
    }
    
    // Clear existing patches array and rebuild with merged data
    existing_patches->clear();
    
    // Process patches from downloaded file
    auto new_patches = new_config["patch"].as_array();
    if (new_patches) {
      for (const auto& new_patch : *new_patches) {
        if (auto* new_table = new_patch.as_table()) {
          // Get name of the new patch
          std::string patch_name;
          if (auto name = new_table->get("name")) {
            if (auto* name_str = name->as_string()) {
              patch_name = name_str->get();
            }
          }
          
          if (patch_name.empty()) {
            // No name, just add it as-is
            toml::table new_patch_copy(*new_table);
            if (!new_patch_copy.contains("is_enabled")) {
              new_patch_copy.insert("is_enabled", false);
            }
            existing_patches->push_back(std::move(new_patch_copy));
            modified = true;
            continue;
          }
          
          // Check if this patch already exists
          auto existing_it = existing_patch_map.find(patch_name);
          if (existing_it == existing_patch_map.end()) {
            // New patch - add it (with is_enabled = false by default)
            toml::table new_patch_copy(*new_table);
            if (!new_patch_copy.contains("is_enabled")) {
              new_patch_copy.insert("is_enabled", false);
            }
            existing_patches->push_back(std::move(new_patch_copy));
            modified = true;
            XELOGI("Added new patch '{}' to {}", patch_name, 
                   existing_path.filename().string());
          } else {
            // Patch exists - check if enabled
            bool is_enabled = patch_enabled_state[patch_name];
            
            if (is_enabled) {
              // Patch is enabled - keep existing version but note if update available
              auto* existing_table = existing_it->second;
              bool has_changes = false;
              
              // Check if any fields differ (to detect updates)
              for (auto&& [key, value] : *new_table) {
                std::string key_str(key.str());
                if (key_str != "is_enabled") {
                  auto existing_value = existing_table->get(key);
                  if (!existing_value || !NodesEqual(*existing_value, value)) {
                    has_changes = true;
                    break;
                  }
                }
              }
              
              if (has_changes) {
                skipped_updates.push_back(patch_name);
              }
              
              // Keep the existing patch as-is
              existing_patches->push_back(*existing_table);
            } else {
              // Patch is disabled - safe to update all fields
              toml::table updated_patch(*new_table);
              updated_patch.insert_or_assign("is_enabled", false);
              
              // Check if actually changed
              auto* existing_table = existing_it->second;
              bool changed = false;
              for (auto&& [key, value] : *new_table) {
                auto existing_value = existing_table->get(key);
                if (!existing_value || !NodesEqual(*existing_value, value)) {
                  changed = true;
                  break;
                }
              }
              
              if (changed) {
                modified = true;
                XELOGI("Updated patch '{}' in {}", patch_name,
                       existing_path.filename().string());
              }
              
              existing_patches->push_back(std::move(updated_patch));
            }
          }
        }
      }
    }
    
    // Report skipped updates
    if (!skipped_updates.empty()) {
      std::string skipped_list;
      for (size_t i = 0; i < skipped_updates.size(); i++) {
        if (i > 0) skipped_list += ", ";
        skipped_list += skipped_updates[i];
      }
      XELOGW("Skipped updating {} enabled patches in {}: {}", 
             skipped_updates.size(), 
             existing_path.filename().string(),
             skipped_list);
    }
    
    // Write back if modified
    if (modified) {
      std::ofstream file(existing_path, std::ios::out | std::ios::trunc);
      if (file.is_open()) {
        file << existing_config;
        file.close();
        return true;
      }
    }
    
    return false;
    
  } catch (const toml::parse_error& err) {
    XELOGE("Failed to parse patch file {}: {}", 
           existing_path.filename().string(), err.description());
    return false;
  } catch (const std::exception& e) {
    XELOGE("Failed to merge patch file {}: {}", 
           existing_path.filename().string(), e.what());
    return false;
  }
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

}  // namespace app
}  // namespace xe