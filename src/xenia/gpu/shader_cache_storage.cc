/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/shader_cache_storage.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"

namespace xe {
namespace gpu {

ShaderCacheStorage::ShaderCacheStorage() = default;

ShaderCacheStorage::~ShaderCacheStorage() { Shutdown(); }

bool ShaderCacheStorage::Initialize(const std::filesystem::path& cache_root,
                                    uint32_t title_id, const char* api_name,
                                    const char* api_suffix,
                                    bool edram_rov_used) {
  Shutdown();

  XELOGI(
      "ShaderCacheStorage: Initializing for title {:08X}, API: {}, suffix: {}",
      title_id, api_name, api_suffix);

  cache_root_ = cache_root;
  title_id_ = title_id;
  api_name_ = api_name;
  api_suffix_ = api_suffix;

  auto shader_storage_root = cache_root / "shaders";
  auto shader_storage_shareable_root = shader_storage_root / "shareable";

  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      XELOGE(
          "Failed to create the shareable shader storage directory, persistent "
          "shader storage will be disabled: {}",
          shader_storage_shareable_root.string());
      return false;
    }
  }

  // Open pipeline storage file
  std::string pipeline_extension;
  if (std::strcmp(api_name, "d3d12") == 0) {
    pipeline_extension = "d3d12.xpso";
  } else if (std::strcmp(api_name, "vulkan") == 0) {
    pipeline_extension = "vulkan.xvso";
  } else {
    pipeline_extension = std::string(api_name) + ".xpso";
  }

  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.{}", title_id, api_suffix, pipeline_extension);

  pipeline_storage_file_ =
      xe::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
  if (!pipeline_storage_file_) {
    XELOGE(
        "ShaderCacheStorage: Failed to open the {} pipeline description "
        "storage file for "
        "writing, persistent shader storage will be disabled: {}",
        api_name, pipeline_storage_file_path.string());
    return false;
  }
  XELOGI("ShaderCacheStorage: Opened pipeline storage file: {}",
         pipeline_storage_file_path.string());

  // Open shader storage file (common between backends)
  auto shader_storage_file_path =
      shader_storage_shareable_root / fmt::format("{:08X}.xsh", title_id);
  shader_storage_file_ =
      xe::filesystem::OpenFile(shader_storage_file_path, "a+b");
  if (!shader_storage_file_) {
    XELOGE(
        "ShaderCacheStorage: Failed to open the guest shader storage file for "
        "writing, persistent "
        "shader storage will be disabled: {}",
        shader_storage_file_path.string());
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    return false;
  }
  XELOGI("ShaderCacheStorage: Opened shader storage file: {}",
         shader_storage_file_path.string());

  ++shader_storage_index_;
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

  // Start the storage writing thread
  storage_write_thread_shutdown_ = false;
  storage_write_thread_ =
      xe::threading::Thread::Create({}, [this]() { StorageWriteThread(); });
  if (!storage_write_thread_) {
    XELOGE("Failed to create shader cache storage thread");
    Shutdown();
    return false;
  }
  storage_write_thread_->set_name("Shader Storage");

  XELOGI("ShaderCacheStorage: Initialization complete, storage thread started");
  return true;
}

void ShaderCacheStorage::Shutdown() {
  XELOGI("ShaderCacheStorage: Shutting down");
  if (storage_write_thread_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_thread_shutdown_ = true;
    }
    storage_write_request_cond_.notify_all();
    xe::threading::Wait(storage_write_thread_.get(), false);
    storage_write_thread_.reset();
  }

  storage_write_shader_queue_.clear();
  storage_write_pipeline_queue_.clear();

  if (pipeline_storage_file_) {
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (shader_storage_file_) {
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    shader_storage_file_flush_needed_ = false;
  }

  cache_root_.clear();
  title_id_ = 0;
  api_name_.clear();
  api_suffix_.clear();
}

void ShaderCacheStorage::QueueShaderWrite(const Shader* shader) {
  if (!shader_storage_file_ || !storage_write_thread_) {
    return;
  }

  XELOGI("ShaderCacheStorage: Queuing shader {:016X} for write (type: {})",
         shader->ucode_data_hash(),
         shader->type() == xenos::ShaderType::kVertex ? "vertex" : "pixel");

  shader_storage_file_flush_needed_ = true;
  {
    std::lock_guard<std::mutex> lock(storage_write_request_lock_);
    storage_write_shader_queue_.push_back(shader);
  }
  storage_write_request_cond_.notify_one();
}

void ShaderCacheStorage::QueuePipelineWrite(const void* pipeline_description,
                                            size_t description_size,
                                            uint64_t description_hash) {
  if (!pipeline_storage_file_ || !storage_write_thread_) {
    return;
  }

  XELOGI("ShaderCacheStorage: Queuing pipeline {:016X} for write",
         description_hash);

  pipeline_storage_file_flush_needed_ = true;
  PipelineWriteRequest request;
  request.hash = description_hash;
  request.data.resize(description_size);
  std::memcpy(request.data.data(), pipeline_description, description_size);

  {
    std::lock_guard<std::mutex> lock(storage_write_request_lock_);
    storage_write_pipeline_queue_.push_back(std::move(request));
  }
  storage_write_request_cond_.notify_one();
}

void ShaderCacheStorage::RequestFlush() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
  }
}

void ShaderCacheStorage::EndSubmission() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    XELOGI("ShaderCacheStorage: EndSubmission - requesting flush");
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }
}

bool ShaderCacheStorage::LoadShaderCache(ShaderLoadCallback* callback) {
  if (!shader_storage_file_) {
    return false;
  }

  XELOGI("ShaderCacheStorage: Loading shader cache from disk");

  // Check for valid header
  struct {
    uint32_t magic;
    uint32_t version_swapped;
  } shader_storage_file_header;

  constexpr uint32_t shader_storage_magic = 0x48534558;  // 'XESH'

  fseek(shader_storage_file_, 0, SEEK_SET);
  if (fread(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
            shader_storage_file_) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      xe::byte_swap(shader_storage_file_header.version_swapped) ==
          ShaderStoredHeader::kVersion) {
    XELOGI("ShaderCacheStorage: Valid shader cache header found");
    uint64_t shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    ShaderStoredHeader shader_header;
    std::vector<uint32_t> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);

    while (true) {
      if (!fread(&shader_header, sizeof(shader_header), 1,
                 shader_storage_file_)) {
        break;
      }

      size_t ucode_byte_count =
          shader_header.ucode_dword_count * sizeof(uint32_t);
      ucode_dwords.resize(shader_header.ucode_dword_count);

      if (shader_header.ucode_dword_count &&
          !fread(ucode_dwords.data(), ucode_byte_count, 1,
                 shader_storage_file_)) {
        break;
      }

      uint64_t ucode_data_hash =
          XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        // Validation failed
        break;
      }

      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;

      XELOGI(
          "ShaderCacheStorage: Loaded shader {:016X} from cache (type: {}, {} "
          "dwords)",
          ucode_data_hash,
          shader_header.type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
          shader_header.ucode_dword_count);

      if (callback) {
        Shader* shader = callback->OnShaderLoaded(
            shader_header.type, ucode_dwords.data(),
            shader_header.ucode_dword_count, ucode_data_hash);

        if (shader && shader->ucode_storage_index() != shader_storage_index_) {
          // Mark as loaded from current storage
          shader->set_ucode_storage_index(shader_storage_index_);
        }
      }
    }

    // Truncate if corrupted
    XELOGI("ShaderCacheStorage: Loaded {} bytes of shader cache",
           shader_storage_valid_bytes);
    xe::filesystem::TruncateStdioFile(shader_storage_file_,
                                      shader_storage_valid_bytes);

  } else {
    // Write new header
    XELOGI("ShaderCacheStorage: Creating new shader cache file");
    xe::filesystem::TruncateStdioFile(shader_storage_file_, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version_swapped =
        xe::byte_swap(ShaderStoredHeader::kVersion);
    fwrite(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
           shader_storage_file_);
  }

  // Seek to end for appending
  fseek(shader_storage_file_, 0, SEEK_END);

  return true;
}

bool ShaderCacheStorage::LoadPipelineCache(PipelineLoadCallback* callback,
                                           size_t pipeline_description_size) {
  if (!pipeline_storage_file_) {
    return false;
  }

  XELOGI(
      "ShaderCacheStorage: Loading pipeline cache from disk (description size: "
      "{})",
      pipeline_description_size);

  // Pipeline storage format is backend-specific, so we'll need different magic
  // values
  uint32_t pipeline_storage_magic;
  uint32_t pipeline_storage_magic_api;

  if (api_name_ == "d3d12") {
    pipeline_storage_magic = 0x53504558;  // 'XEPS'
    pipeline_storage_magic_api =
        (api_suffix_ == "rov") ? 0x4F525844 : 0x54525844;  // 'DXRO' or 'DXRT'
  } else if (api_name_ == "vulkan") {
    pipeline_storage_magic = 0x53505658;  // 'XVPS'
    pipeline_storage_magic_api =
        (api_suffix_ == "rov") ? 0x4F524B56 : 0x54524B56;  // 'VKRO' or 'VKRT'
  } else {
    return false;
  }

  struct {
    uint32_t magic;
    uint32_t magic_api;
    uint32_t version_swapped;
  } pipeline_storage_file_header;

  // For now, use a high version number that both backends should update to
  // match
  const uint32_t pipeline_storage_version_swapped = xe::byte_swap(0xFFFFFFFF);

  fseek(pipeline_storage_file_, 0, SEEK_SET);
  if (fread(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header),
            1, pipeline_storage_file_) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      pipeline_storage_file_header.version_swapped ==
          pipeline_storage_version_swapped) {
    XELOGI("ShaderCacheStorage: Valid pipeline cache header found");
    // Read pipeline descriptions
    xe::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
    int64_t pipeline_storage_told_end =
        xe::filesystem::Tell(pipeline_storage_file_);

    size_t pipeline_count =
        size_t(pipeline_storage_told_end >=
                       int64_t(sizeof(pipeline_storage_file_header))
                   ? (uint64_t(pipeline_storage_told_end) -
                      sizeof(pipeline_storage_file_header)) /
                         (sizeof(uint64_t) + pipeline_description_size)
                   : 0);

    if (pipeline_count &&
        xe::filesystem::Seek(pipeline_storage_file_,
                             int64_t(sizeof(pipeline_storage_file_header)),
                             SEEK_SET)) {
      std::vector<uint8_t> buffer(sizeof(uint64_t) + pipeline_description_size);
      size_t valid_pipelines = 0;

      for (size_t i = 0; i < pipeline_count; ++i) {
        if (!fread(buffer.data(), buffer.size(), 1, pipeline_storage_file_)) {
          break;
        }

        uint64_t description_hash = *reinterpret_cast<uint64_t*>(buffer.data());
        const void* description = buffer.data() + sizeof(uint64_t);

        // Validate hash
        if (XXH3_64bits(description, pipeline_description_size) !=
            description_hash) {
          break;
        }

        valid_pipelines = i + 1;

        XELOGI("ShaderCacheStorage: Loaded pipeline {:016X} from cache",
               description_hash);

        if (callback) {
          callback->OnPipelineLoaded(description, pipeline_description_size,
                                     description_hash);
        }
      }

      // Truncate if corrupted
      XELOGI("ShaderCacheStorage: Loaded {} pipelines from cache",
             valid_pipelines);
      xe::filesystem::TruncateStdioFile(pipeline_storage_file_,
                                        sizeof(pipeline_storage_file_header) +
                                            valid_pipelines * buffer.size());
    }
  } else {
    // Write new header
    XELOGI("ShaderCacheStorage: Creating new pipeline cache file");
    xe::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version_swapped =
        pipeline_storage_version_swapped;
    fwrite(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header),
           1, pipeline_storage_file_);
  }

  // Seek to end for appending
  fseek(pipeline_storage_file_, 0, SEEK_END);

  return true;
}

void ShaderCacheStorage::StorageWriteThread() {
  XELOGI("ShaderCacheStorage: Storage write thread started");
  ShaderStoredHeader shader_header;
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<uint32_t> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      if (shader_storage_file_) {
        XELOGI("ShaderCacheStorage: Flushing shader cache to disk");
        fflush(shader_storage_file_);
      }
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      if (pipeline_storage_file_) {
        XELOGI("ShaderCacheStorage: Flushing pipeline cache to disk");
        fflush(pipeline_storage_file_);
      }
    }

    const Shader* shader = nullptr;
    PipelineWriteRequest pipeline_request;
    bool has_pipeline = false;

    {
      std::unique_lock<std::mutex> lock(storage_write_request_lock_);
      if (storage_write_thread_shutdown_) {
        XELOGI("ShaderCacheStorage: Storage write thread shutting down");
        return;
      }

      if (!storage_write_shader_queue_.empty()) {
        shader = storage_write_shader_queue_.front();
        storage_write_shader_queue_.pop_front();
      } else if (storage_write_flush_shaders_) {
        storage_write_flush_shaders_ = false;
        flush_shaders = true;
      }

      if (!storage_write_pipeline_queue_.empty()) {
        pipeline_request = std::move(storage_write_pipeline_queue_.front());
        storage_write_pipeline_queue_.pop_front();
        has_pipeline = true;
      } else if (storage_write_flush_pipelines_) {
        storage_write_flush_pipelines_ = false;
        flush_pipelines = true;
      }

      if (!shader && !has_pipeline && !flush_shaders && !flush_pipelines) {
        storage_write_request_cond_.wait(lock);
        continue;
      }
    }

    if (shader && shader_storage_file_) {
      shader_header.ucode_data_hash = shader->ucode_data_hash();
      shader_header.ucode_dword_count = shader->ucode_dword_count();
      shader_header.type = shader->type();

      XELOGI(
          "ShaderCacheStorage: Writing shader {:016X} to disk (type: {}, {} "
          "dwords)",
          shader_header.ucode_data_hash,
          shader_header.type == xenos::ShaderType::kVertex ? "vertex" : "pixel",
          shader_header.ucode_dword_count);

      fwrite(&shader_header, sizeof(shader_header), 1, shader_storage_file_);

      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // Need to swap because the hash is calculated for the shader with guest
        // endianness
        xe::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                          shader_header.ucode_dword_count);
        fwrite(ucode_guest_endian.data(),
               shader_header.ucode_dword_count * sizeof(uint32_t), 1,
               shader_storage_file_);
      }
    }

    if (has_pipeline && pipeline_storage_file_) {
      XELOGI("ShaderCacheStorage: Writing pipeline {:016X} to disk",
             pipeline_request.hash);
      // Write hash followed by data
      fwrite(&pipeline_request.hash, sizeof(pipeline_request.hash), 1,
             pipeline_storage_file_);
      fwrite(pipeline_request.data.data(), pipeline_request.data.size(), 1,
             pipeline_storage_file_);
    }
  }
}

}  // namespace gpu
}  // namespace xe
