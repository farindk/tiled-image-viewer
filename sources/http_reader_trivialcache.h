/*
 * HTTP Range Request Reader for libheif (Trivial Cache Implementation)
 *
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#ifndef HTTP_READER_TRIVIALCACHE_H
#define HTTP_READER_TRIVIALCACHE_H

#include <libheif/heif.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Lightweight range info for visualization (without data)
struct RangeInfo;

// Forward declaration of implementation
struct HttpReaderTrivialCacheImpl;

class HttpReader_TrivialCache {
public:
  HttpReader_TrivialCache();
  ~HttpReader_TrivialCache();

  // Non-copyable
  HttpReader_TrivialCache(const HttpReader_TrivialCache&) = delete;
  HttpReader_TrivialCache& operator=(const HttpReader_TrivialCache&) = delete;

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

private:
  std::unique_ptr<HttpReaderTrivialCacheImpl> m_impl;
  heif_reader m_heif_reader;
};

#endif
