/*
 * HTTP Range Request Reader for libheif
 *
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#include "http_reader.h"
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

// --- CURL write callback ---

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t total_size = size * nmemb;
  std::vector<uint8_t>* buffer = static_cast<std::vector<uint8_t>*>(userp);
  uint8_t* data = static_cast<uint8_t*>(contents);
  buffer->insert(buffer->end(), data, data + total_size);
  return total_size;
}

// --- Helper to fetch a range from URL ---

static bool fetch_range(HttpReader* ctx, uint64_t start, uint64_t end, std::vector<uint8_t>& out_data)
{
  CURL* curl = static_cast<CURL*>(ctx->curl_handle);
  if (!curl) {
    ctx->last_error = "CURL handle not initialized";
    return false;
  }

  out_data.clear();

  char range_str[64];
  snprintf(range_str, sizeof(range_str), "%lu-%lu", (unsigned long)start, (unsigned long)end);

  curl_easy_setopt(curl, CURLOPT_URL, ctx->url.c_str());
  curl_easy_setopt(curl, CURLOPT_RANGE, range_str);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_data);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode res = curl_easy_perform(curl);

  // Reset range for next request
  curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);

  if (res != CURLE_OK) {
    ctx->last_error = curl_easy_strerror(res);
    return false;
  }

  return true;
}

// --- heif_reader callbacks ---

static int64_t http_get_position(void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);
  return ctx->current_position;
}

static int http_read(void* data, size_t size, void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);

  if (ctx->current_position + (int64_t)size > ctx->file_size) {
    return heif_reader_grow_status_size_beyond_eof;
  }

  uint8_t* out = static_cast<uint8_t*>(data);
  size_t remaining = size;
  uint64_t pos = ctx->current_position;

  while (remaining > 0) {
    bool found = false;

    // Search in cache
    for (auto& range : ctx->cache) {
      if (pos >= range.start && pos < range.start + range.data.size()) {
        uint64_t offset_in_range = pos - range.start;
        size_t available = range.data.size() - offset_in_range;
        size_t to_copy = std::min(remaining, available);

        memcpy(out, range.data.data() + offset_in_range, to_copy);
        out += to_copy;
        pos += to_copy;
        remaining -= to_copy;
        found = true;
        break;
      }
    }

    if (!found) {
      // Fetch missing data - fetch a reasonable chunk
      uint64_t fetch_start = pos;
      uint64_t fetch_end = std::min(pos + 65536 - 1, (uint64_t)(ctx->file_size - 1));

      std::vector<uint8_t> fetched_data;
      if (!fetch_range(ctx, fetch_start, fetch_end, fetched_data)) {
        return heif_reader_grow_status_size_beyond_eof;
      }

      CachedRange new_range;
      new_range.start = fetch_start;
      new_range.data = std::move(fetched_data);
      ctx->cache.push_back(std::move(new_range));
    }
  }

  ctx->current_position = pos;
  return heif_reader_grow_status_size_reached;
}

static int http_seek(int64_t position, void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);

  if (position < 0 || position > ctx->file_size) {
    return -1;
  }

  ctx->current_position = position;
  return 0;
}

static enum heif_reader_grow_status http_wait_for_file_size(int64_t target_size, void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);

  if (target_size <= ctx->file_size) {
    return heif_reader_grow_status_size_reached;
  }
  return heif_reader_grow_status_size_beyond_eof;
}

static struct heif_reader_range_request_result http_request_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);

  struct heif_reader_range_request_result result;
  result.reader_error_code = 0;
  result.reader_error_msg = nullptr;
  result.status = heif_reader_grow_status_size_reached;
  result.range_end = end;

  // Note: end is exclusive (one byte after last position)
  uint64_t last_byte = end > 0 ? end - 1 : 0;

  // Check if already in cache
  for (auto& range : ctx->cache) {
    if (start >= range.start && last_byte < range.start + range.data.size()) {
      // Already cached
      return result;
    }
  }

  // Fetch the range (curl uses inclusive end)
  std::vector<uint8_t> fetched_data;
  if (!fetch_range(ctx, start, last_byte, fetched_data)) {
    result.status = heif_reader_grow_status_error;
    result.reader_error_code = 1;
    return result;
  }

  CachedRange new_range;
  new_range.start = start;
  new_range.data = std::move(fetched_data);
  ctx->cache.push_back(std::move(new_range));

  return result;
}

static void http_release_file_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReader* ctx = static_cast<HttpReader*>(userdata);
  std::lock_guard<std::mutex> lock(ctx->mutex);

  // Remove cached ranges that fall within the specified range
  ctx->cache.erase(
    std::remove_if(ctx->cache.begin(), ctx->cache.end(),
      [start, end](const CachedRange& r) {
        return r.start >= start && r.start + r.data.size() - 1 <= end;
      }),
    ctx->cache.end()
  );
}

static void http_release_error_msg(const char* msg)
{
  // No-op: we use std::string internally
  (void)msg;
}

// --- Static heif_reader struct ---

static struct heif_reader http_reader = {
  .reader_api_version = 2,
  .get_position = http_get_position,
  .read = http_read,
  .seek = http_seek,
  .wait_for_file_size = http_wait_for_file_size,
  .request_range = http_request_range,
  .preload_range_hint = nullptr,
  .release_file_range = http_release_file_range,
  .release_error_msg = http_release_error_msg
};

// --- Public interface ---

bool http_reader_init(HttpReader* ctx, const char* url)
{
  ctx->url = url;
  ctx->file_size = -1;
  ctx->current_position = 0;
  ctx->cache.clear();
  ctx->last_error.clear();

  // Initialize CURL
  ctx->curl_handle = curl_easy_init();
  if (!ctx->curl_handle) {
    ctx->last_error = "Failed to initialize CURL";
    return false;
  }

  CURL* curl = static_cast<CURL*>(ctx->curl_handle);

  // Perform HEAD request to get file size
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    ctx->last_error = curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    ctx->curl_handle = nullptr;
    return false;
  }

  curl_off_t content_length = 0;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

  if (content_length <= 0) {
    ctx->last_error = "Could not determine file size (server may not support range requests)";
    curl_easy_cleanup(curl);
    ctx->curl_handle = nullptr;
    return false;
  }

  ctx->file_size = content_length;

  // Reset NOBODY for subsequent requests
  curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

  printf("HTTP: File size is %ld bytes\n", (long)ctx->file_size);

  return true;
}

void http_reader_cleanup(HttpReader* ctx)
{
  if (ctx->curl_handle) {
    curl_easy_cleanup(static_cast<CURL*>(ctx->curl_handle));
    ctx->curl_handle = nullptr;
  }
  ctx->cache.clear();
}

const heif_reader* get_http_reader()
{
  return &http_reader;
}

int64_t http_reader_get_file_size(HttpReader* ctx)
{
  std::lock_guard<std::mutex> lock(ctx->mutex);
  return ctx->file_size;
}

std::vector<RangeInfo> http_reader_get_cached_ranges(HttpReader* ctx)
{
  std::lock_guard<std::mutex> lock(ctx->mutex);
  std::vector<RangeInfo> ranges;
  ranges.reserve(ctx->cache.size());
  for (const auto& r : ctx->cache) {
    ranges.push_back({r.start, r.data.size()});
  }
  return ranges;
}
