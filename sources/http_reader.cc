/*
 * HTTP Range Request Reader for libheif
 *
 * Copyright (c) 2024 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#include "http_reader.h"
#include <curl/curl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <mutex>

// --- Implementation struct (PIMPL) ---

struct CachedRange {
  uint64_t start;
  std::vector<uint8_t> data;
};

struct HttpReaderImpl {
  std::string url;
  int64_t file_size = -1;
  int64_t current_position = 0;
  void* curl_handle = nullptr;
  std::vector<CachedRange> cache;
  mutable std::mutex mutex;
  std::string last_error;

  bool fetch_range(uint64_t start, uint64_t end, std::vector<uint8_t>& out_data);
};

// --- CURL write callback ---

static size_t http_curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t total_size = size * nmemb;
  std::vector<uint8_t>* buffer = static_cast<std::vector<uint8_t>*>(userp);
  uint8_t* data = static_cast<uint8_t*>(contents);
  buffer->insert(buffer->end(), data, data + total_size);
  return total_size;
}

// --- HttpReaderImpl helper ---

bool HttpReaderImpl::fetch_range(uint64_t start, uint64_t end, std::vector<uint8_t>& out_data)
{
  CURL* curl = static_cast<CURL*>(curl_handle);
  if (!curl) {
    last_error = "CURL handle not initialized";
    return false;
  }

  out_data.clear();

  printf("fetch pos: %d, size: %d\n", start, end-start);

  char range_str[64];
  snprintf(range_str, sizeof(range_str), "%lu-%lu", (unsigned long)start, (unsigned long)end);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_RANGE, range_str);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_curl_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_data);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode res = curl_easy_perform(curl);

  // Reset range for next request
  curl_easy_setopt(curl, CURLOPT_RANGE, nullptr);

  if (res != CURLE_OK) {
    last_error = curl_easy_strerror(res);
    return false;
  }

  return true;
}

// --- heif_reader callbacks (userdata is HttpReaderImpl*) ---

static int64_t cb_get_position(void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->current_position;
}

static int cb_read(void* data, size_t size, void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (impl->current_position + (int64_t)size > impl->file_size) {
    return heif_reader_grow_status_size_beyond_eof;
  }

  uint8_t* out = static_cast<uint8_t*>(data);
  size_t remaining = size;
  uint64_t pos = impl->current_position;

  while (remaining > 0) {
    bool found = false;

    // Search in cache
    for (auto& range : impl->cache) {
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
      uint64_t fetch_end = std::min(pos + 65536 - 1, (uint64_t)(impl->file_size - 1));

      std::vector<uint8_t> fetched_data;
      if (!impl->fetch_range(fetch_start, fetch_end, fetched_data)) {
        return heif_reader_grow_status_size_beyond_eof;
      }

      CachedRange new_range;
      new_range.start = fetch_start;
      new_range.data = std::move(fetched_data);
      impl->cache.push_back(std::move(new_range));
    }
  }

  impl->current_position = pos;
  return heif_reader_grow_status_size_reached;
}

static int cb_seek(int64_t position, void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (position < 0 || position > impl->file_size) {
    return -1;
  }

  impl->current_position = position;
  return 0;
}

static enum heif_reader_grow_status cb_wait_for_file_size(int64_t target_size, void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (target_size <= impl->file_size) {
    return heif_reader_grow_status_size_reached;
  }
  return heif_reader_grow_status_size_beyond_eof;
}

static struct heif_reader_range_request_result cb_request_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  struct heif_reader_range_request_result result;
  result.reader_error_code = 0;
  result.reader_error_msg = nullptr;
  result.status = heif_reader_grow_status_size_reached;
  result.range_end = end;

  // Note: end is exclusive (one byte after last position)
  uint64_t last_byte = end > 0 ? end - 1 : 0;

  // Check if already in cache
  for (auto& range : impl->cache) {
    if (start >= range.start && last_byte < range.start + range.data.size()) {
      // Already cached
      return result;
    }
  }

  // Fetch the range (curl uses inclusive end)
  std::vector<uint8_t> fetched_data;
  if (!impl->fetch_range(start, last_byte, fetched_data)) {
    result.status = heif_reader_grow_status_error;
    result.reader_error_code = 1;
    return result;
  }

  CachedRange new_range;
  new_range.start = start;
  new_range.data = std::move(fetched_data);
  impl->cache.push_back(std::move(new_range));

  return result;
}

static void cb_release_file_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReaderImpl* impl = static_cast<HttpReaderImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  // Remove cached ranges that fall within the specified range
  impl->cache.erase(
    std::remove_if(impl->cache.begin(), impl->cache.end(),
      [start, end](const CachedRange& r) {
        return r.start >= start && r.start + r.data.size() - 1 <= end;
      }),
    impl->cache.end()
  );
}

static void cb_release_error_msg(const char* msg)
{
  // No-op: we use std::string internally
  (void)msg;
}

// --- HttpReader class implementation ---

HttpReader::HttpReader()
  : m_impl(std::make_unique<HttpReaderImpl>())
{
  // Initialize heif_reader struct for this instance
  m_heif_reader.reader_api_version = 2;
  m_heif_reader.get_position = cb_get_position;
  m_heif_reader.read = cb_read;
  m_heif_reader.seek = cb_seek;
  m_heif_reader.wait_for_file_size = cb_wait_for_file_size;
  m_heif_reader.request_range = cb_request_range;
  m_heif_reader.preload_range_hint = nullptr;
  m_heif_reader.release_file_range = cb_release_file_range;
  m_heif_reader.release_error_msg = cb_release_error_msg;
}

HttpReader::~HttpReader()
{
  cleanup();
}

bool HttpReader::init(const char* url)
{
  m_impl->url = url;
  m_impl->file_size = -1;
  m_impl->current_position = 0;
  m_impl->cache.clear();
  m_impl->last_error.clear();

  // Initialize CURL
  m_impl->curl_handle = curl_easy_init();
  if (!m_impl->curl_handle) {
    m_impl->last_error = "Failed to initialize CURL";
    return false;
  }

  CURL* curl = static_cast<CURL*>(m_impl->curl_handle);

  // Perform HEAD request to get file size
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    m_impl->last_error = curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    m_impl->curl_handle = nullptr;
    return false;
  }

  curl_off_t content_length = 0;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);

  if (content_length <= 0) {
    m_impl->last_error = "Could not determine file size (server may not support range requests)";
    curl_easy_cleanup(curl);
    m_impl->curl_handle = nullptr;
    return false;
  }

  m_impl->file_size = content_length;

  // Reset NOBODY for subsequent requests
  curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

  printf("HTTP: File size is %ld bytes\n", (long)m_impl->file_size);

  return true;
}

void HttpReader::cleanup()
{
  if (m_impl && m_impl->curl_handle) {
    curl_easy_cleanup(static_cast<CURL*>(m_impl->curl_handle));
    m_impl->curl_handle = nullptr;
  }
  if (m_impl) {
    m_impl->cache.clear();
  }
}

const heif_reader* HttpReader::get_heif_reader() const
{
  return &m_heif_reader;
}

void* HttpReader::get_callback_user_data() const
{
  return m_impl.get();
}

int64_t HttpReader::get_file_size() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->file_size;
}

std::vector<RangeInfo> HttpReader::get_cached_ranges() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  std::vector<RangeInfo> ranges;
  ranges.reserve(m_impl->cache.size());
  for (const auto& r : m_impl->cache) {
    ranges.push_back({r.start, r.data.size()});
  }
  return ranges;
}
