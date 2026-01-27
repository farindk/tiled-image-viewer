/*
 * HTTP Range Request Reader for libheif
 *
 * Copyright (c) 2024 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#ifndef HTTP_READER_H
#define HTTP_READER_H

#include <libheif/heif.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct CachedRange {
  uint64_t start;
  std::vector<uint8_t> data;
};

// Lightweight range info for visualization (without data)
struct RangeInfo {
  uint64_t start;
  uint64_t size;
};

struct HttpReader {
  std::string url;
  int64_t file_size = -1;
  int64_t current_position = 0;
  void* curl_handle = nullptr;
  std::vector<CachedRange> cache;
  std::mutex mutex;
  std::string last_error;
};

// Initialize context with URL (performs HEAD request to get file size)
bool http_reader_init(HttpReader* ctx, const char* url);

// Cleanup resources
void http_reader_cleanup(HttpReader* ctx);

// Get the heif_reader struct for HTTP access
const heif_reader* get_http_reader();

// Get file size for visualization
int64_t http_reader_get_file_size(HttpReader* ctx);

// Get a snapshot of cached ranges for visualization (thread-safe)
std::vector<RangeInfo> http_reader_get_cached_ranges(HttpReader* ctx);

#endif
