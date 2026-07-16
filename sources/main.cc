/*
 * Tiled-Image-Viewer example application for libheif.
 *
 * Copyright (c) 2024 Dirk Farin <dirk.farin@gmail.com>
 *
 * Tiled-Image-Viewer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Tiled-Image-Viewer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libheif/heif.h>
#include <libheif/heif_uncompressed.h>
#include <raylib.h>

#include <cmath>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <cassert>
#include <memory>
#include <getopt.h>
#include "http_reader_blockcache.h"
#include "http_reader_trivialcache.h"
#include "tile_loader.h"


int window_width = 2000;
int window_height = 2000;

int tile_cache_size = 150;

bool process_transformations = true;

// Auto-detected from the first decoded tile (set in main, read by load_tile).
bool is_float_mono = false;

float float_min = 0.0f;
float float_max = 1.0f;

const int heif_unci_component_type_monochrome = 1; // ISO/IEC 23001-17 Table 1 - Component Types

enum class tile_state
{
  loading,
  waiting_for_texture_upload,
  ready
};

struct Tile
{
  int x, y;
  uint32_t layer;
  tile_state state = tile_state::loading;
  Texture2D texture;
  Image image;
};

std::vector<Tile> tiles;
std::mutex tilemutex;   // this locks all operations on the 'tiles' vector

int tile_width, tile_height; // Tile size in signed integer (for computing with negative coordinates)

heif_context* ctx;

std::vector<heif_image_handle*> pymd_layer_handles;
uint32_t active_layer;

heif_image_tiling tiling;

std::unique_ptr<TileLoader> tile_loader;

void move_tile_to_front_of_lru_cache(size_t idx)
{
  Tile t = tiles[idx];
  for (size_t i = idx; i > 0; i--) {
    tiles[i] = tiles[i - 1];
  }

  tiles[0] = t;
}


std::mutex loadmutex;

void load_tile(int tx, int ty, int layer)
{
  printf("loading Tile %d;%d, layer: %d\n", tx, ty, layer);

  std::lock_guard<std::mutex> lock(loadmutex);

  heif_image* img;

  heif_decoding_options* options = heif_decoding_options_alloc();
  options->ignore_transformations = !process_transformations;

  heif_colorspace decode_colorspace = is_float_mono ? heif_colorspace_undefined : heif_colorspace_RGB;
  heif_chroma decode_chroma = is_float_mono ? heif_chroma_undefined : heif_chroma_interleaved_RGBA;

  heif_error err = heif_image_handle_decode_image_tile(pymd_layer_handles[layer], &img, decode_colorspace, decode_chroma, options, tx, ty);
  heif_decoding_options_free(options);

  if (err.code) {
    printf("heif_decode_image error: %s\n", err.message);
    exit(0);
  }

  Color* pixels = (Color*) malloc(tile_width * tile_height * sizeof(Color));

  if (is_float_mono) {
    // Find the 32-bit floating-point monochrome component and read it via the float32 API.
    uint32_t n = heif_image_get_number_of_used_components(img);
    uint32_t ids[16];
    if (n > 16) n = 16;
    heif_image_get_used_component_ids(img, ids);

    uint32_t float_comp_idx = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
      uint16_t ctype = heif_image_get_component_type(img, ids[i]);
      heif_component_datatype dtype = heif_image_get_component_datatype(img, ids[i]);
      int bpp = heif_image_get_component_bits_per_pixel(img, ids[i]);
      if (ctype == heif_unci_component_type_monochrome &&
          dtype == heif_component_datatype_floating_point &&
          bpp == 32) {
        float_comp_idx = ids[i];
        break;
      }
    }

    if (float_comp_idx == UINT32_MAX) {
      printf("Tile %d;%d (layer %d) has no monochrome 32-bit component; cannot apply float mapping\n", tx, ty, layer);
      memset(pixels, 0, tile_width * tile_height * sizeof(Color));
    }
    else {
      size_t byte_stride = 0;
      const float* data = heif_image_get_component_float32_readonly(img, float_comp_idx, &byte_stride);
      size_t float_stride = byte_stride / sizeof(float);
      float scale = 255.0f / (float_max - float_min);

      for (int y = 0; y < tile_height; y++) {
        const float* row = data + (size_t) y * float_stride;
        for (int x = 0; x < tile_width; x++) {
          float v = (row[x] - float_min) * scale;
          if (v < 0.0f) v = 0.0f;
          else if (v > 255.0f) v = 255.0f;
          uint8_t g = (uint8_t) v;
          pixels[y * tile_width + x] = {g, g, g, 255};
        }
      }
    }
  }
  else {
    int stride;
    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

    for (int y = 0; y < tile_height; y++) {
      memcpy(&pixels[y * tile_width], data + y * stride, tile_width * 4);
    }
  }

  Image image = {
      .data = pixels,
      .width = tile_width,
      .height = tile_height,
      .mipmaps = 1,
      .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
  };

  tilemutex.lock();
  for (auto& tile : tiles) {
    if (tile.x == tx && tile.y == ty && tile.layer == layer) {
      tile.state = tile_state::waiting_for_texture_upload;
      tile.image = image;
      break;
    }
  }
  tilemutex.unlock();

  // clean up resources
  heif_image_release(img);
}


static struct option long_options[] = {
    {(char* const) "trivial-reader",  no_argument,       0, 't'},
    {(char* const) "no-transforms",   no_argument,       0, 'T'},
    {(char* const) "url",             no_argument,       0, 'u'},
    {(char* const) "primary",         no_argument,       0, 'p'},
    {(char* const) "block-size",      required_argument, 0, 'b'},
    {(char* const) "float-min",       required_argument, 0, 1000},
    {(char* const) "float-max",       required_argument, 0, 1001},
    {(char* const) "help",            no_argument,       0, 'h'},
    {0, 0,                                               0, 0}
};

void show_help(const char* argv0)
{
  fprintf(stderr, " tiled-image-viewer      (c) Dirk Farin\n");
  fprintf(stderr, "----------------------------------------\n");
  fprintf(stderr, "usage: tiled-image-viewer [options] image.heif\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr, "  -t, --trivial-reader   use trivial cache reader instead of block cache\n");
  fprintf(stderr, "      --no-transforms    do not process HEIF image transformations\n");
  fprintf(stderr, "  -u, --url              treat input as HTTP/HTTPS URL\n");
  fprintf(stderr, "  -p, --primary          start with primary image (if not given, start at overview image)\n");
  fprintf(stderr, "  -b, --block-size <kB>  block size in kB for block cache reader (default: 64)\n");
  fprintf(stderr, "      --float-min <v>    for float monochrome images: lower bound mapped to 0 (default 0.0)\n");
  fprintf(stderr, "      --float-max <v>    for float monochrome images: upper bound mapped to 255 (default 1.0)\n");
  fprintf(stderr, "  -h, --help             show help\n");
}

int main(int argc, char** argv)
{
  SetTraceLogLevel(LOG_ERROR);

  bool use_url_mode = false;
  bool start_at_primary = false;
  int block_size_kb = 64;  // default: 64KB block cache
  bool use_trivial_reader = false;

  while (true) {
    int option_index = 0;
    int c = getopt_long(argc, argv, "tupb:h", long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
      case 't':
        use_trivial_reader = true;
        break;
      case 'T':
        process_transformations = false;
        break;
      case 'u':
        use_url_mode = true;
        break;
      case 'p':
        start_at_primary = true;
        break;
      case 'b':
        block_size_kb = atoi(optarg);
        if (block_size_kb <= 0) {
          fprintf(stderr, "Error: block size must be a positive integer\n");
          return 1;
        }
        break;
      case 1000:
        float_min = (float) atof(optarg);
        break;
      case 1001:
        float_max = (float) atof(optarg);
        break;
      case 'h':
        show_help(argv[0]);
        return 0;
    }
  }

  if (float_max <= float_min) {
    fprintf(stderr, "Error: --float-max must be greater than --float-min\n");
    return 1;
  }

  if (optind != argc - 1) {
    show_help(argv[0]);
    return 0;
  }

  const char* input_filename = argv[optind];

  ctx = heif_context_alloc();

  // --- remove security limit to be able to load extremely large 'grid' images

  const heif_security_limits* no_limits = heif_get_disabled_security_limits();
  heif_context_set_security_limits(ctx, no_limits);

  // --- load and parse input file

  printf("loading ...\n");

  std::unique_ptr<HttpReader_BlockCache> block_reader;
  std::unique_ptr<HttpReader_TrivialCache> trivial_reader;
  const heif_reader* reader = nullptr;
  void* reader_userdata = nullptr;
  heif_error err;

  if (use_url_mode) {
    if (!use_trivial_reader) {
      block_reader = std::make_unique<HttpReader_BlockCache>(block_size_kb * 1024);
      if (!block_reader->init(input_filename)) {
        fprintf(stderr, "Cannot connect to URL: %s\n", input_filename);
        exit(10);
      }
      reader = block_reader->get_heif_reader();
      reader_userdata = block_reader->get_callback_user_data();
    } else {
      trivial_reader = std::make_unique<HttpReader_TrivialCache>();
      if (!trivial_reader->init(input_filename)) {
        fprintf(stderr, "Cannot connect to URL: %s\n", input_filename);
        exit(10);
      }
      reader = trivial_reader->get_heif_reader();
      reader_userdata = trivial_reader->get_callback_user_data();
    }
    err = heif_context_read_from_reader(ctx, reader, reader_userdata, nullptr);
  } else {
    err = heif_context_read_from_file(ctx, input_filename, nullptr);
  }

  if (err.code) {
    fprintf(stderr, "Cannot load file: %s\n", err.message);
    exit(10);
  }

  printf("loading finished\n");

  // --- get the ID of the primary image

  heif_item_id  primary_id;
  err = heif_context_get_primary_image_ID(ctx, &primary_id);
  if (err.code) {
    fprintf(stderr, "Cannot get primary image: %s\n", err.message);
    exit(10);
  }


  // --- Load multi-resolution pyramid if there is one.

  int nGroups;
  struct heif_entity_group* groups = heif_context_get_entity_groups(ctx, heif_fourcc('p', 'y', 'm', 'd'), primary_id, &nGroups);
  if (nGroups > 0) {
    assert(nGroups == 1);
    pymd_layer_handles.resize(groups[0].num_entities);

    for (uint32_t i = 0; i < groups[0].num_entities; i++) {
      uint32_t layer_image_id = groups[0].entities[i];
      heif_context_get_image_handle(ctx, layer_image_id, &pymd_layer_handles[i]);
      if (start_at_primary && layer_image_id == primary_id) {
        active_layer = i;
      }
    }

    if (!start_at_primary) {
      active_layer = 0; // always start at overview layer
    }
  }
  else {
    // Build dummy pyramid of only one image

    pymd_layer_handles.resize(1);
    heif_context_get_image_handle(ctx, primary_id, &pymd_layer_handles[0]);
    active_layer = 0;
  }
  heif_entity_groups_release(groups, nGroups);


  // --- Get tiling information for active layer

  heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);
  tile_width = (int)tiling.tile_width;
  tile_height = (int)tiling.tile_height;

  printf("tilesize: %u x %u\n", tiling.tile_width, tiling.tile_height);
  printf("tiles: %u x %u\n", tiling.num_columns, tiling.num_rows);


  // --- Probe tile (0,0) of the active layer with native colorspace to detect a
  //     monochrome float32 component. The result selects the decode parameters
  //     used throughout the session, so we do this once instead of per tile.
  {
    heif_image* probe = nullptr;
    heif_decoding_options* probe_opts = heif_decoding_options_alloc();
    probe_opts->ignore_transformations = !process_transformations;
    heif_error probe_err = heif_image_handle_decode_image_tile(pymd_layer_handles[active_layer],
                                                               &probe,
                                                               heif_colorspace_undefined,
                                                               heif_chroma_undefined,
                                                               probe_opts, 0, 0);
    heif_decoding_options_free(probe_opts);
    if (!probe_err.code) {
      uint32_t n = heif_image_get_number_of_used_components(probe);
      if (n == 1) {
        uint32_t id = 0;
        heif_image_get_used_component_ids(probe, &id);
        if (heif_image_get_component_type(probe, id) == heif_unci_component_type_monochrome &&
            heif_image_get_component_datatype(probe, id) == heif_component_datatype_floating_point &&
            heif_image_get_component_bits_per_pixel(probe, id) == 32) {
          is_float_mono = true;
          printf("detected float32 monochrome image; mapping range [%g..%g] to grayscale\n",
                 float_min, float_max);
        }
      }
      heif_image_release(probe);
    }
  }


  // --- Display image and interaction loop

  InitWindow(window_width, window_height, "Tiled HEIF Image Viewer    (c) Dirk Farin");
  int image_width = tiling.tile_width * tiling.num_columns;
  int image_height = tiling.tile_height * tiling.num_rows;
  int x00 = (image_width - window_width) / 2;
  int y00 = (image_height - window_height) / 2;
  int mx = 0, my = 0;
  int dx = 0, dy = 0;
  bool mouse_pressed = false;

  SetTargetFPS(50);

  // Create tile loader with load_tile as callback
  tile_loader = std::make_unique<TileLoader>(load_tile, 1);

  while (!WindowShouldClose()) {

    // Mark all queued tiles as unwanted before scanning visible tiles
    tile_loader->mark_all_unwanted();

    BeginDrawing();
    ClearBackground({50, 50, 50, 255});

    // --- Mouse zooming with mouse wheel

    float wheel = GetMouseWheelMove();  // 0, 1, -1

    if (wheel > 0 && active_layer < pymd_layer_handles.size() - 1) {
      active_layer++;

      heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);
      tile_width = (int)tiling.tile_width;
      tile_height = (int)tiling.tile_height;

      int m_x = GetMouseX();
      int m_y = GetMouseY();

      x00 = (x00 + m_x) * 2 - m_x;
      y00 = (y00 + m_y) * 2 - m_y;
    }
    else if (wheel < 0 && active_layer > 0) {
      active_layer--;

      heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);
      tile_width = (int)tiling.tile_width;
      tile_height = (int)tiling.tile_height;

      int m_x = GetMouseX();
      int m_y = GetMouseY();

      x00 = (x00 + m_x) / 2 - m_x;
      y00 = (y00 + m_y) / 2 - m_y;
    }

    // --- Mouse panning

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      mx = GetMouseX();
      my = GetMouseY();
      dx = dy = 0;
      mouse_pressed = true;
    }
    else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      x00 -= dx;
      y00 -= dy;
      dx = dy = 0;
      mouse_pressed = false;
    }
    else if (mouse_pressed) {
      dx = GetMouseX() - mx;
      dy = GetMouseY() - my;
    }

    int x0 = x00 - dx;
    int y0 = y00 - dy;

    int tile_idx_x0 = x0 / tile_width;
    int tile_idx_y0 = y0 / tile_height;

    // --- Draw all tiles visible on screen

    tilemutex.lock();

    for (int ty = tile_idx_y0; ty * tile_height - y0 < window_height; ty++) {
      for (int tx = tile_idx_x0; tx * tile_width - x0 < window_width; tx++) {

        if (tx < 0 || tx >= tiling.num_columns)
          continue;

        if (ty < 0 || ty >= tiling.num_rows)
          continue;

        bool tile_found = false;

        for (size_t i = 0; i < tiles.size(); i++) {
          if (tiles[i].x == tx && tiles[i].y == ty && tiles[i].layer == active_layer) {
            tile_found = true;
            if (tiles[i].state == tile_state::ready) {
              DrawTexture(tiles[i].texture, tx * tile_width - x0, ty * tile_height - y0, WHITE);
              move_tile_to_front_of_lru_cache(i);
            }
            else if (tiles[i].state == tile_state::waiting_for_texture_upload) {
              tiles[i].texture = LoadTextureFromImage(tiles[i].image);
              UnloadImage(tiles[i].image);
              tiles[i].state = tile_state::ready;
              DrawTexture(tiles[i].texture, tx * tile_width - x0, ty * tile_height - y0, WHITE);
              move_tile_to_front_of_lru_cache(i);
            }
          }

          DrawRectangleLines(tx * tile_width - x0, ty * tile_height - y0, tile_width, tile_height, WHITE);
        }

        // --- If the tile is not loaded yet, load it in the background

        if (!tile_found) {
          if (tiles.size() == tile_cache_size) {
            if (tiles.back().state == tile_state::ready) {
              UnloadTexture(tiles.back().texture);
            }
            if (tiles.back().state == tile_state::waiting_for_texture_upload) {
              UnloadImage(tiles.back().image);
            }

            tiles.pop_back();
          }

          Tile t;
          t.x = tx;
          t.y = ty;
          t.layer = active_layer;
          tiles.push_back(t);
          move_tile_to_front_of_lru_cache(tiles.size() - 1);

          // Queue tile for loading (prioritized)
          tile_loader->queue_tile(tx, ty, active_layer);
        }
      }
    }
    tilemutex.unlock();

    // --- Draw HTTP download progress bar (only in URL mode)
    if (use_url_mode) {
      const int bar_height = 16;
      const int bar_y = 0;

      int64_t file_size = block_reader ? block_reader->get_file_size() : trivial_reader->get_file_size();
      if (file_size > 0) {
        // Draw red background (not downloaded)
        DrawRectangle(0, bar_y, window_width, bar_height, RED);

        // Draw green for downloaded ranges
        auto ranges = block_reader ? block_reader->get_cached_ranges() : trivial_reader->get_cached_ranges();
        for (const auto& r : ranges) {
          int x_start = (int)((r.start * window_width) / file_size);
          int x_end = (int)(((r.start + r.size) * window_width) / file_size);
          DrawRectangle(x_start, bar_y, x_end - x_start, bar_height, GREEN);
        }
      }
    }

    EndDrawing();
  }

  // Shutdown tile loader before closing window
  tile_loader->shutdown();
  tile_loader.reset();

  CloseWindow();

  heif_context_free(ctx);

  return 0;
}
