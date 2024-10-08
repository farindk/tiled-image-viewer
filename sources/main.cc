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
#include <raylib.h>

#include <cmath>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <cstring>
#include <cassert>

int window_width = 2000;
int window_height = 2000;

int tile_cache_size = 150;

bool process_transformations = true;

enum class tile_state
{
  loading,
  waiting_for_texture_load,
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

  heif_error err = heif_image_handle_decode_image_tile(pymd_layer_handles[layer], &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options, tx, ty);
  heif_decoding_options_free(options);

  if (err.code) {
    printf("heif_decode_image error: %s\n", err.message);
    exit(0);
  }

  int stride;
  const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

  Color* pixels = (Color*) malloc(tile_width * tile_height * sizeof(Color));

  // Fill the image with RGB pixels
  for (int y = 0; y < tile_height; y++) {
    memcpy(&pixels[y * tile_width], data + y * stride, tile_width * 4);
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
      tile.state = tile_state::waiting_for_texture_load;
      tile.image = image;
      break;
    }
  }
  tilemutex.unlock();

  // clean up resources
  heif_image_release(img);
}


int main(int argc, char** argv)
{
  SetTraceLogLevel(LOG_ERROR);

  const char* input_filename = argv[1];

  ctx = heif_context_alloc();

  // --- remove security limit to be able to load extremely large 'grid' images

  heif_security_limits* limits = heif_context_get_security_limits(ctx);
  limits->max_children_per_box = 0;

  // --- load and parse input file

  printf("loading ...\n");

  heif_error err = heif_context_read_from_file(ctx, input_filename, nullptr);
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
      if (layer_image_id == primary_id) {
        active_layer = i;
      }
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


  // --- Display image and interaction loop

  InitWindow(window_width, window_height, "libheif-viewer");
  int x00 = 0, y00 = 0;
  int mx = 0, my = 0;
  int dx = 0, dy = 0;
  bool mouse_pressed = false;

  SetTargetFPS(50);

  while (!WindowShouldClose()) {

    BeginDrawing();
    ClearBackground({0, 0, 0, 255});

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
            else if (tiles[i].state == tile_state::waiting_for_texture_load) {
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
            if (tiles.back().state == tile_state::waiting_for_texture_load) {
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

          // load Tile
          std::thread loadingThread(load_tile, tx, ty, active_layer);
          loadingThread.detach();
        }
      }
    }
    tilemutex.unlock();

    EndDrawing();
  }

  CloseWindow();

  heif_context_free(ctx);

  return 0;
}
