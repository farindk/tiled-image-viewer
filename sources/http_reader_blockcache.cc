/*
 * HTTP Range Request Reader for libheif (Block Cache Implementation)
 *
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of Tiled-Image-Viewer.
 */

#include "http_reader_blockcache.h"
#include <curl/curl.h>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <cassert>

// --- Implementation struct (PIMPL) ---

struct CachedBlock
{
  std::vector<uint8_t> data;
};

struct HttpReaderBlockCacheImpl
{
  std::string url;
  int64_t file_size = -1;
  int64_t current_position = 0;
  void* curl_handle = nullptr;
  std::vector<CachedBlock> cache;
  mutable std::mutex mutex;
  std::string last_error;
  uint32_t block_size = 64 * 1024;

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

// --- HttpReaderBlockCacheImpl helper ---

bool HttpReaderBlockCacheImpl::fetch_range(uint64_t start, uint64_t end, std::vector<uint8_t>& out_data)
{
  CURL* curl = static_cast<CURL*>(curl_handle);
  if (!curl) {
    last_error = "CURL handle not initialized";
    return false;
  }

  out_data.clear();

  printf("fetch pos: %lu, size: %lu\n", (unsigned long) start, (unsigned long) (end - start + 1));

  char range_str[64];
  snprintf(range_str, sizeof(range_str), "%lu-%lu", (unsigned long) start, (unsigned long) end);

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

// --- heif_reader callbacks (userdata is HttpReaderBlockCacheImpl*) ---

static int64_t cb_get_position(void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->current_position;
}

static int cb_read(void* data, size_t size, void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (impl->current_position + (int64_t) size > impl->file_size) {
    return heif_reader_grow_status_size_beyond_eof;
  }

  uint8_t* out = static_cast<uint8_t*>(data);
  int remaining = (int) size;
  uint64_t pos = impl->current_position;
  uint32_t block_size = impl->block_size;

  bool found = true;

  int first_block = pos / block_size;
  int last_block = (pos + size - 1) / block_size;
  for (int b = first_block; b <= last_block; b++) {
    if (impl->cache[b].data.empty()) {
      found = false;
      break;
    }
  }

  if (found) {
    for (int b = first_block; b <= last_block; b++) {
      int block_start_pos = b * block_size;
      int block_end_pos = block_start_pos + block_size;

      int max_block_copy = block_end_pos - pos;
      int to_copy = std::min(remaining, max_block_copy);

      int offset_in_block = pos - block_start_pos;

      memcpy(out, impl->cache[b].data.data() + offset_in_block, to_copy);
      out += to_copy;
      pos += to_copy;
      remaining -= to_copy;
    }
  }
  else {
    // Fetch missing data - fetch a reasonable chunk
    uint64_t fetch_start = pos;
    uint64_t fetch_end = std::min(pos + 65536 - 1, (uint64_t) (impl->file_size - 1));

    std::vector<uint8_t> fetched_data;
    if (!impl->fetch_range(fetch_start, fetch_end, fetched_data)) {
      return heif_reader_grow_status_size_beyond_eof;
    }

    //CachedRange new_range;
    //new_range.start = fetch_start;
    //new_range.data = std::move(fetched_data);
    //impl->cache.push_back(std::move(new_range));

    assert(false);
  }

  impl->current_position = pos;
  return heif_reader_grow_status_size_reached;
}

static int cb_seek(int64_t position, void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (position < 0 || position > impl->file_size) {
    return -1;
  }

  impl->current_position = position;
  return 0;
}

static enum heif_reader_grow_status cb_wait_for_file_size(int64_t target_size, void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  if (target_size <= impl->file_size) {
    return heif_reader_grow_status_size_reached;
  }
  return heif_reader_grow_status_size_beyond_eof;
}

static struct heif_reader_range_request_result cb_request_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

  struct heif_reader_range_request_result result;
  result.reader_error_code = 0;
  result.reader_error_msg = nullptr;
  result.status = heif_reader_grow_status_size_reached;
  result.range_end = end;

  uint32_t block_size = impl->block_size;

  // Note: end is exclusive (one byte after last position)
  uint64_t last_byte = end > 0 ? end - 1 : 0;

  // Extend to full block

  int start_block = start / block_size;
  int last_block = last_byte / block_size;

  while (start_block <= last_block) {
    if (!impl->cache[start_block].data.empty()) {
      start_block++;
    }
    else {
      break;
    }
  }

  while (start_block <= last_block) {
    if (!impl->cache[last_block].data.empty()) {
      last_block--;
    }
    else {
      break;
    }
  }

  // Check if already in cache
  if (start_block > last_block) {
    return result;
  }


  // Fetch the range

  start = start_block * block_size;
  end = last_block * block_size + block_size;

  if (end > (uint64_t) impl->file_size) {
    end = impl->file_size;
  }

  last_byte = end - 1;

  // Fetch the range (curl uses inclusive end)
  std::vector<uint8_t> fetched_data;
  if (!impl->fetch_range(start, last_byte, fetched_data)) {
    result.status = heif_reader_grow_status_error;
    result.reader_error_code = 1;
    return result;
  }

  for (int block = start_block; block <= last_block; block++) {
    size_t block_data_offset = (block - start_block) * block_size;
    size_t bytes_to_copy = std::min((size_t) block_size, fetched_data.size() - block_data_offset);

    impl->cache[block].data.resize(bytes_to_copy);
    memcpy(impl->cache[block].data.data(), fetched_data.data() + block_data_offset, bytes_to_copy);
  }

  return result;
}

static void cb_release_file_range(uint64_t start, uint64_t end, void* userdata)
{
  HttpReaderBlockCacheImpl* impl = static_cast<HttpReaderBlockCacheImpl*>(userdata);
  std::lock_guard<std::mutex> lock(impl->mutex);

#if 0
  // Remove cached ranges that fall within the specified range
  impl->cache.erase(
                    std::remove_if(impl->cache.begin(), impl->cache.end(),
                                   [start, end](const CachedBlock& r) {
                                     return r.start >= start && r.start + r.data.size() - 1 <= end;
                                   }),
                    impl->cache.end()
                   );
#endif
}

static void cb_release_error_msg(const char* msg)
{
  // No-op: we use std::string internally
  (void) msg;
}

// --- HttpReader_BlockCache class implementation ---

HttpReader_BlockCache::HttpReader_BlockCache(uint32_t block_size)
  : m_impl(std::make_unique<HttpReaderBlockCacheImpl>())
  , m_block_size(block_size)
{
  m_impl->block_size = block_size;

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

HttpReader_BlockCache::~HttpReader_BlockCache()
{
  cleanup();
}

bool HttpReader_BlockCache::init(const char* url)
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
  m_impl->cache.resize((content_length + m_block_size - 1) / m_block_size);

  // Reset NOBODY for subsequent requests
  curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

  printf("HTTP: File size is %ld bytes (block size: %u)\n", (long) m_impl->file_size, m_block_size);

  return true;
}

void HttpReader_BlockCache::cleanup()
{
  if (m_impl && m_impl->curl_handle) {
    curl_easy_cleanup(static_cast<CURL*>(m_impl->curl_handle));
    m_impl->curl_handle = nullptr;
  }
  if (m_impl) {
    m_impl->cache.clear();
  }
}

const heif_reader* HttpReader_BlockCache::get_heif_reader() const
{
  return &m_heif_reader;
}

void* HttpReader_BlockCache::get_callback_user_data() const
{
  return m_impl.get();
}

int64_t HttpReader_BlockCache::get_file_size() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->file_size;
}

std::vector<RangeInfo> HttpReader_BlockCache::get_cached_ranges() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  std::vector<RangeInfo> ranges;
  ranges.reserve(m_impl->cache.size());

  for (size_t b = 0; b < m_impl->cache.size(); b++) {
    if (!m_impl->cache[b].data.empty()) {
      ranges.push_back({b * m_block_size, m_block_size});
    }
  }

  return ranges;
}
