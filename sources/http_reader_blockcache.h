/*
 * HTTP Range Request Reader for libheif (Block Cache Implementation)
 *
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#ifndef HTTP_READER_BLOCKCACHE_H
#define HTTP_READER_BLOCKCACHE_H

#include <libheif/heif.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Lightweight range info for visualization (without data)
struct RangeInfo {
  uint64_t start;
  uint64_t size;
};

// Forward declaration of implementation
struct HttpReaderBlockCacheImpl;

class HttpReader_BlockCache {
public:
  HttpReader_BlockCache(uint32_t block_size = 64 * 1024);
  ~HttpReader_BlockCache();

  // Non-copyable
  HttpReader_BlockCache(const HttpReader_BlockCache&) = delete;
  HttpReader_BlockCache& operator=(const HttpReader_BlockCache&) = delete;

  // Initialize with URL (performs HEAD request to get file size)
  bool init(const char* url);

  // Cleanup resources
  void cleanup();

  // Get the heif_reader struct for HTTP access (instance-specific)
  const heif_reader* get_heif_reader() const;

  // Get userdata pointer for heif_reader callbacks
  void* get_callback_user_data() const;

  // Get file size for visualization
  int64_t get_file_size() const;

  // Get a snapshot of cached ranges for visualization (thread-safe)
  std::vector<RangeInfo> get_cached_ranges() const;

  // Get block size
  uint32_t get_block_size() const { return m_block_size; }

private:
  std::unique_ptr<HttpReaderBlockCacheImpl> m_impl;
  heif_reader m_heif_reader;
  uint32_t m_block_size;
};

#endif
