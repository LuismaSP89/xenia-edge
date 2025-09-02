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
    LoadPatchFiles();
  }
}

void PatchesDialog::LoadPatchFiles() {
  patch_displays_.clear();

  if (!std::filesystem::exists(patches_directory_)) {
    std::filesystem::create_directories(patches_directory_);
    XELOGI("Created patches directory: {}", patches_directory_.string());
  }

  if (!patch_db_) {
    patch_db_ = std::make_unique<patcher::PatchDB>(
        emulator_window_->emulator()->storage_root());
  } else {
    // Reload patches from disk to get fresh data
    patch_db_->LoadPatches();
  }

  auto& all_patches = patch_db_->GetAllPatches();
  XELOGI("Found {} patch files", all_patches.size());

  for (const auto& patch_file : all_patches) {
    PatchDisplay display;
    display.title_id = patch_file.title_id;
    display.title_name = patch_file.title_name;
    display.filename = patch_file.filename;

    // Extract version info from filename (e.g., "(TU3)" or mark as base)
    size_t tu_pos = display.filename.find("(TU");
    if (tu_pos != std::string::npos) {
      size_t end_pos = display.filename.find(')', tu_pos);
      if (end_pos != std::string::npos) {
        display.version_info =
            display.filename.substr(tu_pos + 1, end_pos - tu_pos - 1);
      }
    } else {
      // Check if there are other versions with TU for this title
      bool has_tu_versions = false;
      for (const auto& other_patch : all_patches) {
        if (other_patch.title_id == patch_file.title_id &&
            other_patch.filename.find("(TU") != std::string::npos) {
          has_tu_versions = true;
          break;
        }
      }
      display.version_info = has_tu_versions ? "Base" : "";
    }

    display.is_expanded = false;

    for (size_t i = 0; i < patch_file.patch_info.size(); ++i) {
      PatchInfo info;
      info.id = static_cast<uint32_t>(i);
      info.name = patch_file.patch_info[i].patch_name;
      info.description = patch_file.patch_info[i].patch_desc;
      info.author = patch_file.patch_info[i].patch_author;
      info.is_enabled = patch_file.patch_info[i].is_enabled;
      display.patches.push_back(std::move(info));
    }

    patch_displays_.push_back(std::move(display));
  }

  // Sort patch displays alphabetically by title name
  std::sort(patch_displays_.begin(), patch_displays_.end(),
            [](const PatchDisplay& a, const PatchDisplay& b) {
              return a.title_name < b.title_name;
            });
}

void PatchesDialog::SavePatchSettings() {
  XELOGI("SavePatchSettings called, saving {} displays",
         patch_displays_.size());

  for (auto& display : patch_displays_) {
    std::filesystem::path patch_file_path =
        patches_directory_ / display.filename;

    XELOGI("Saving to patch file: {}", patch_file_path.string());

    if (!std::filesystem::exists(patch_file_path)) {
      XELOGW("Patch file not found: {}", patch_file_path.string());
      continue;
    }

    try {
      auto config = toml::parse_file(patch_file_path.string());

      auto patches = config["patch"].as_array();
      if (patches) {
        XELOGI("Found {} patches in config, display has {} patches",
               patches->size(), display.patches.size());
        size_t index = 0;
        for (auto& patch : *patches) {
          if (patch.is_table() && index < display.patches.size()) {
            // Update or add the is_enabled field
            auto* table = patch.as_table();
            bool enabled = display.patches[index].is_enabled;
            XELOGI("Setting patch {} '{}' is_enabled to {}", index,
                   display.patches[index].name, enabled);
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
        XELOGI("Saved patch settings for {:08X}", display.title_id);
      } else {
        XELOGE("Failed to open patch file for writing: {}",
               patch_file_path.string());
      }

    } catch (const toml::parse_error& err) {
      XELOGE("Failed to parse patch file for {:08X}: {}", display.title_id,
             err.description());
    } catch (const std::exception& e) {
      XELOGE("Failed to save patch settings for {:08X}: {}", display.title_id,
             e.what());
    }
  }

  // Reload the patch database to reflect changes
  if (patch_db_) {
    patch_db_->LoadPatches();
  }

  needs_reload_ = true;
}

void PatchesDialog::ReloadPatchDatabase() {
  if (patch_db_) {
    patch_db_->LoadPatches();
    LoadPatchFiles();
  }
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
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove)) {
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
  ImGui::InputTextWithHint("##patch_filter", "Filter by title or patch name...",
                           filter_text_.data(), filter_text_.size());

  if (ImGui::Button("Refresh")) {
    LoadPatchFiles();
  }

  ImGui::SameLine();
  if (ImGui::Button("Open Patches Directory")) {
    if (!std::filesystem::exists(patches_directory_)) {
      std::filesystem::create_directories(patches_directory_);
    }
    LaunchFileExplorer(patches_directory_);
  }

  ImGui::Separator();

  if (patch_displays_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "No patch files found in: %s",
                       patches_directory_.string().c_str());
    ImGui::TextWrapped(
        "Place .patch.toml files in the patches directory. "
        "Patch files should be named with the title ID "
        "(e.g., 4D530910.patch.toml)");
  } else {
    ImGui::BeginChild("PatchList", ImVec2(0, -30), true);

    for (auto& display : patch_displays_) {
      std::string filter_lower(filter_text_.data());
      std::transform(filter_lower.begin(), filter_lower.end(),
                     filter_lower.begin(), ::tolower);

      bool title_matches = false;

      if (!filter_lower.empty()) {
        std::string title_lower = display.title_name;
        std::transform(title_lower.begin(), title_lower.end(),
                       title_lower.begin(), ::tolower);

        title_matches = title_lower.find(filter_lower) != std::string::npos;

        if (!title_matches) {
          bool has_patch_match = false;
          for (const auto& patch : display.patches) {
            std::string patch_lower = patch.name;
            std::transform(patch_lower.begin(), patch_lower.end(),
                           patch_lower.begin(), ::tolower);
            if (patch_lower.find(filter_lower) != std::string::npos) {
              has_patch_match = true;
              break;
            }
          }

          if (!has_patch_match) {
            continue;  // Skip this game entirely
          }
        }
      }

      ImGui::PushID(fmt::format("patch_game_{:08X}", display.title_id).c_str());

      // Auto-expand if filter is active and there are matches
      bool should_expand = !filter_lower.empty();
      ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_None;
      if (should_expand) {
        node_flags |= ImGuiTreeNodeFlags_DefaultOpen;
      }

      std::string tree_label;
      if (!display.version_info.empty()) {
        tree_label = fmt::format("{} [{}] ({:08X})", display.title_name,
                                 display.version_info, display.title_id);
      } else {
        tree_label =
            fmt::format("{} ({:08X})", display.title_name, display.title_id);
      }

      if (ImGui::TreeNodeEx(tree_label.c_str(), node_flags)) {
        if (!display.patches.empty()) {
          int visible_patches = 0;
          for (auto& patch : display.patches) {
            // Only filter individual patches if:
            // - There's a filter active
            // - The game title doesn't match (so we need to filter patches)
            if (!filter_lower.empty() && !title_matches) {
              std::string patch_lower = patch.name;
              std::transform(patch_lower.begin(), patch_lower.end(),
                             patch_lower.begin(), ::tolower);
              if (patch_lower.find(filter_lower) == std::string::npos) {
                continue;  // Skip this patch if it doesn't match the filter
              }
            }

            visible_patches++;
            ImGui::PushID(fmt::format("patch_item_{}", patch.id).c_str());

            if (ImGui::Checkbox(patch.name.c_str(), &patch.is_enabled)) {
              XELOGI("Patch '{}' toggled to {}", patch.name, patch.is_enabled);
              SavePatchSettings();
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
