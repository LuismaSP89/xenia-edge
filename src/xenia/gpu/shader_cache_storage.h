/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADER_CACHE_STORAGE_H_
#define XENIA_GPU_SHADER_CACHE_STORAGE_H_

#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Common shader cache storage implementation for both D3D12 and Vulkan backends
class ShaderCacheStorage {
 public:
  // Common header structure for shader storage
  struct ShaderStoredHeader {
    uint64_t ucode_data_hash;
    uint32_t ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr uint32_t kVersion = 0x20201219;
  };

  // Pipeline description - backend-specific, passed as opaque data
  struct PipelineStoredDescription {
    uint64_t description_hash;
    // Backend-specific description follows
  };

  ShaderCacheStorage();
  ~ShaderCacheStorage();

  // Initialize storage for a specific title and backend
  bool Initialize(const std::filesystem::path& cache_root, uint32_t title_id,
                  const char* api_name,    // "d3d12" or "vulkan"
                  const char* api_suffix,  // "rov"/"rtv" etc
                  bool edram_rov_used);

  void Shutdown();

  // Queue operations for the storage thread
  void QueueShaderWrite(const Shader* shader);
  void QueuePipelineWrite(const void* pipeline_description,
                          size_t description_size, uint64_t description_hash);

  // Request flush of pending writes
  void RequestFlush();

  // Called at end of submission to flush if needed
  void EndSubmission();

  // Read operations (synchronous, called during initialization)
  class ShaderLoadCallback {
   public:
    virtual ~ShaderLoadCallback() = default;
    // Return nullptr to skip, or the loaded shader to track it
    virtual Shader* OnShaderLoaded(xenos::ShaderType type,
                                   const uint32_t* ucode_dwords,
                                   size_t ucode_dword_count,
                                   uint64_t ucode_data_hash) = 0;
  };

  class PipelineLoadCallback {
   public:
    virtual ~PipelineLoadCallback() = default;
    virtual void OnPipelineLoaded(const void* description_data,
                                  size_t description_size,
                                  uint64_t description_hash) = 0;
  };

  // Load existing cache (called during initialization)
  bool LoadShaderCache(ShaderLoadCallback* callback);
  bool LoadPipelineCache(PipelineLoadCallback* callback,
                         size_t pipeline_description_size);

  // Get current storage index for deduplication
  uint32_t shader_storage_index() const { return shader_storage_index_; }

 private:
  // Storage thread that handles all disk I/O
  void StorageWriteThread();

  // File handles
  FILE* shader_storage_file_ = nullptr;
  FILE* pipeline_storage_file_ = nullptr;

  // Storage metadata
  std::filesystem::path cache_root_;
  uint32_t title_id_ = 0;
  std::string api_name_;
  std::string api_suffix_;
  uint32_t shader_storage_index_ = 0;

  // Write thread and synchronization
  std::unique_ptr<xe::threading::Thread> storage_write_thread_;
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  bool storage_write_thread_shutdown_ = false;

  // Write queues
  std::deque<const Shader*> storage_write_shader_queue_;
  struct PipelineWriteRequest {
    std::vector<uint8_t> data;
    uint64_t hash;
  };
  std::deque<PipelineWriteRequest> storage_write_pipeline_queue_;

  // Flush control
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool shader_storage_file_flush_needed_ = false;
  bool pipeline_storage_file_flush_needed_ = false;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHADER_CACHE_STORAGE_H_
