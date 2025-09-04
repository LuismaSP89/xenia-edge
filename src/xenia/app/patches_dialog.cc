/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/patches_dialog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"

#ifdef XE_PLATFORM_WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace xe {
namespace app {

PatchesDialog::PatchesDialog(ui::ImGuiDrawer* imgui_drawer,
                             EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  auto emulator = emulator_window_->emulator();
  if (emulator) {
    patches_directory_ = emulator->storage_root() / "patches";
    patch_db_ = std::make_unique<patcher::PatchDB>(emulator->storage_root());
    patch_downloader_ = std::make_unique<PatchDownloader>();
    LoadPatchFiles();
  }
}

void PatchesDialog::LoadPatchFiles() {
  title_patches_.clear();

  if (!std::filesystem::exists(patches_directory_)) {
    std::filesystem::create_directories(patches_directory_);
    XELOGI("Created patches directory");
  }

  auto& all_patches = patch_db_->GetAllPatches();

  for (const auto& patch_file : all_patches) {
    TitlePatchData data;
    data.title_id = patch_file.title_id;
    data.title_name = patch_file.title_name;
    data.filename = patch_file.filename;
    data.hashes = patch_file.hashes;
    data.is_expanded = false;
    
    auto patch_file_path = patches_directory_ / patch_file.filename;
    if (std::filesystem::exists(patch_file_path)) {
      data.file_size = std::filesystem::file_size(patch_file_path);
    }

    for (size_t i = 0; i < patch_file.patch_info.size(); ++i) {
      PatchInfo info;
      info.id = static_cast<uint32_t>(i);
      info.name = patch_file.patch_info[i].patch_name;
      info.description = patch_file.patch_info[i].patch_desc;
      info.author = patch_file.patch_info[i].patch_author;
      info.is_enabled = patch_file.patch_info[i].is_enabled;
      data.patches.push_back(std::move(info));
    }

    title_patches_.push_back(std::move(data));
  }

  // Sort patches by title name
  std::sort(title_patches_.begin(), title_patches_.end(),
            [](const TitlePatchData& a, const TitlePatchData& b) {
              return a.title_name < b.title_name;
            });
}

void PatchesDialog::SaveSinglePatchFile(TitlePatchData& patch_data) {
  std::filesystem::path patch_file_path =
      patches_directory_ / patch_data.filename;

  if (!std::filesystem::exists(patch_file_path)) {
    return;
  }

  try {
    auto config = toml::parse_file(patch_file_path.string());

    auto patches = config["patch"].as_array();
    if (patches) {
      size_t index = 0;
      for (auto& patch : *patches) {
        if (patch.is_table() && index < patch_data.patches.size()) {
          // Update or add the is_enabled field
          auto* table = patch.as_table();
          bool enabled = patch_data.patches[index].is_enabled;
          table->insert_or_assign("is_enabled", enabled);
          index++;
        }
      }
    } else {
      XELOGW("No 'patch' array found in config");
    }

    // Write the updated config back to file
    std::ofstream file(patch_file_path, std::ios::out | std::ios::trunc);
    if (file.is_open()) {
      file << config;
      file.close();
      XELOGI("Saved patch settings for {} ({:08X})", patch_data.filename,
             patch_data.title_id);
    }

  } catch (const toml::parse_error& err) {
    XELOGE("Failed to parse patch file for {:08X}: {}", patch_data.title_id,
           err.description());
  } catch (const std::exception& e) {
    XELOGE("Failed to save patch settings for {:08X}: {}", patch_data.title_id,
           e.what());
  }

  needs_reload_ = true;
}

void PatchesDialog::SavePatchSettings() {
  for (auto& patch_data : title_patches_) {
    SaveSinglePatchFile(patch_data);
  }
}

void PatchesDialog::ReloadPatchDatabase() {
  if (patch_db_) {
    patch_db_->LoadPatches();
    LoadPatchFiles();
  }
}

void PatchesDialog::StartPatchDownload() {
  if (!patch_downloader_ || is_downloading_) {
    return;
  }

  is_downloading_ = true;
  download_current_ = 0;
  download_total_ = 0;
  download_status_ = "Downloading patches from GitHub...";

  // Create patches directory if it doesn't exist
  if (!std::filesystem::exists(patches_directory_)) {
    std::filesystem::create_directories(patches_directory_);
  }

  // Build map of existing files
  std::unordered_map<std::string, size_t> existing_files;
  for (const auto& patch : title_patches_) {
    if (patch.file_size > 0) {
      existing_files[patch.filename] = patch.file_size;
    }
  }
  
  patch_downloader_->DownloadAllPatches(
      patches_directory_,
      existing_files,
      [this](size_t current, size_t total) {
        // Progress callback - runs in background thread
        download_current_ = current;
        download_total_ = total;
        if (total > 0) {
          download_status_ = fmt::format("Processing patches... {}/{}", current, total);
        }
      },
      [this](bool success, const std::string& status) {
        // Completion callback - runs in background thread
        is_downloading_ = false;
        download_status_ = status;
        if (success) {
          // Reload patches after download
          ReloadPatchDatabase();
        }
      });
}

void PatchesDialog::OnDraw(ImGuiIO& io) {
  if (!emulator_window_->emulator()) {
    ImGui::Text("Emulator not initialized");
    return;
  }

  // Set window size
  ImVec2 window_size = ImVec2(800, 600);
  ImGui::SetNextWindowSize(window_size, ImGuiCond_FirstUseEver);

  // Center the window - calculate position based on viewport
  ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
  ImVec2 window_pos = ImVec2((viewport_size.x - window_size.x) * 0.5f,
                             (viewport_size.y - window_size.y) * 0.5f);
  ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);

  bool window_open = true;
  if (!ImGui::Begin("Patch Manager", &window_open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize)) {
    ImGui::End();
    return;
  }

  if (!window_open) {
    ImGui::End();
    Close();
    return;
  }

  ImGui::TextWrapped(
      "Manage game patches. Enable or disable patches for "
      "individual games. Changes will take effect on next game "
      "launch.");
  ImGui::Separator();

  if (filter_text_.size() < 256) {
    filter_text_.resize(256);
  }

  // Set focus to filter input when dialog first opens
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }

  ImGui::SetNextItemWidth(-1);  // Use full width
  ImGui::InputTextWithHint(
      "##patch_filter",
      "Filter by title, media ID, or patch name... (Esc to clear)",
      filter_text_.data(), filter_text_.size(),
      ImGuiInputTextFlags_EscapeClearsAll);

  if (ImGui::Button("Open Patches Directory")) {
    if (!std::filesystem::exists(patches_directory_)) {
      std::filesystem::create_directories(patches_directory_);
    }
    LaunchFileExplorer(patches_directory_);
  }

  ImGui::SameLine();
  if (is_downloading_) {
    ImGui::BeginDisabled();
    ImGui::Button("Download Patches");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Downloading: %zu/%zu", download_current_, download_total_);
  } else {
    if (ImGui::Button("Download Patches")) {
      StartPatchDownload();
    }
  }

  if (!download_status_.empty()) {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s",
                       download_status_.c_str());
  }

  ImGui::Separator();

  if (title_patches_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "No patch files found in: %s",
                       patches_directory_.string().c_str());
    ImGui::TextWrapped(
        "Place .patch.toml files in the patches directory. "
        "Patch files should be named with the title ID "
        "(e.g., 4D530910.patch.toml)");
  } else {
    ImGui::BeginChild("PatchList", ImVec2(0, -30), true);

    for (auto& patch_data : title_patches_) {
      // Parse filter into tokens for fuzzy search
      std::string filter_lower(filter_text_.data());
      std::transform(filter_lower.begin(), filter_lower.end(),
                     filter_lower.begin(), ::tolower);

      std::vector<std::string> filter_tokens;
      if (!filter_lower.empty()) {
        std::string current_token;
        for (char c : filter_lower) {
          if (c == ' ' && !current_token.empty()) {
            filter_tokens.push_back(current_token);
            current_token.clear();
          } else if (c != ' ') {
            current_token += c;
          }
        }
        if (!current_token.empty()) {
          filter_tokens.push_back(current_token);
        }
      }

      // Check if any patches match the filter
      if (!filter_tokens.empty()) {
        bool has_visible_patches = false;
        // Create base searchable text for this game (title + filename)
        std::string base_search_text =
            patch_data.title_name + " " + patch_data.filename;
        std::transform(base_search_text.begin(), base_search_text.end(),
                       base_search_text.begin(), ::tolower);

        for (const auto& patch : patch_data.patches) {
          // Create combined searchable text for each patch
          std::string patch_search_text = base_search_text + " " + patch.name;
          std::transform(patch_search_text.begin(), patch_search_text.end(),
                         patch_search_text.begin(), ::tolower);

          // Check if all filter tokens match
          bool all_tokens_match = true;
          for (const auto& token : filter_tokens) {
            if (patch_search_text.find(token) == std::string::npos) {
              all_tokens_match = false;
              break;
            }
          }

          if (all_tokens_match) {
            has_visible_patches = true;
            break;
          }
        }

        // Skip this game if no patches match
        if (!has_visible_patches) {
          continue;
        }
      }

      // Create unique ID from title_id and hashes
      std::string unique_id = fmt::format("{:08X}", patch_data.title_id);
      for (const auto& hash : patch_data.hashes) {
        unique_id += fmt::format("_{:016X}", hash);
      }
      ImGui::PushID(fmt::format("patch_game_{}", unique_id).c_str());

      // Auto-expand if filter is active and there are matches
      bool should_expand = !filter_lower.empty();
      ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_None;
      if (should_expand) {
        node_flags |= ImGuiTreeNodeFlags_DefaultOpen;
      }

      // Remove .patch.toml extension for display
      std::string display_name = patch_data.filename;
      const std::string patch_toml_ext = ".patch.toml";
      const std::string toml_ext = ".toml";

      if (display_name.size() > patch_toml_ext.size() &&
          display_name.substr(display_name.size() - patch_toml_ext.size()) ==
              patch_toml_ext) {
        display_name.resize(display_name.size() - patch_toml_ext.size());
      } else if (display_name.size() > toml_ext.size() &&
                 display_name.substr(display_name.size() - toml_ext.size()) ==
                     toml_ext) {
        display_name.resize(display_name.size() - toml_ext.size());
      }

      if (ImGui::TreeNodeEx(display_name.c_str(), node_flags)) {
        if (!patch_data.patches.empty()) {
          int visible_patches = 0;

          // Prepare base searchable text if filtering
          std::string base_search_text;
          if (!filter_tokens.empty()) {
            base_search_text =
                patch_data.title_name + " " + patch_data.filename;
            std::transform(base_search_text.begin(), base_search_text.end(),
                           base_search_text.begin(), ::tolower);
          }

          for (auto& patch : patch_data.patches) {
            // Filter individual patches if there's an active filter
            if (!filter_tokens.empty()) {
              // Create combined searchable text for this patch
              std::string patch_search_text =
                  base_search_text + " " + patch.name;
              std::transform(patch_search_text.begin(), patch_search_text.end(),
                             patch_search_text.begin(), ::tolower);

              // Check if all filter tokens match
              bool all_tokens_match = true;
              for (const auto& token : filter_tokens) {
                if (patch_search_text.find(token) == std::string::npos) {
                  all_tokens_match = false;
                  break;
                }
              }

              if (!all_tokens_match) {
                continue;  // Skip this patch if not all tokens match
              }
            }

            visible_patches++;
            ImGui::PushID(fmt::format("patch_item_{}", patch.id).c_str());

            if (ImGui::Checkbox(patch.name.c_str(), &patch.is_enabled)) {
              XELOGI("\"{}\" {}", patch.name,
                     patch.is_enabled ? "enabled" : "disabled");
              SaveSinglePatchFile(patch_data);
            }

            if (!patch.description.empty()) {
              ImGui::SameLine();
              ImGui::TextDisabled("(?)");
              if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextWrapped("%s", patch.description.c_str());
                if (!patch.author.empty()) {
                  ImGui::Text("Author: %s", patch.author.c_str());
                }
                ImGui::EndTooltip();
              }
            }

            ImGui::PopID();
          }

          if (visible_patches == 0) {
            ImGui::TextDisabled("No patches match the filter");
          }
        } else {
          ImGui::TextDisabled("No patches defined in this file");
        }

        ImGui::TreePop();
      }

      ImGui::PopID();
    }

    ImGui::EndChild();
  }

  if (needs_reload_) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "Changes will take effect on next game launch");
  }

  ImGui::End();
}

}  // namespace app
}  // namespace xe
