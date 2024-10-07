
#include <raylib.h>
#include <unistd.h>
#include <math.h>
#include <iostream>
#include <libheif/heif.h>
#include <vector>
#include <thread>
#include <mutex>
#include <string.h>
#include <cassert>

int window_width = 2000;
int window_height = 2000;

int max_tiles = 150;

bool process_transformations = true;

enum class tile_state {
  loading,
  waiting_for_texture_load,
  ready
};

struct tile
{
  int x,y;
  int layer;
  tile_state state = tile_state::loading;
  Texture2D texture;
  Image image;
};

std::vector<tile> tiles;
std::mutex tilemutex;

int tw,th; // tile size

heif_context* ctx;

std::vector<heif_item_id> pymd_layers;
std::vector<heif_image_handle*> pymd_layer_handles;
int active_layer;

heif_image_tiling tiling;

void move_to_front(int idx)
{
  tile t = tiles[idx];
  for (size_t i=idx;i>0;i--) {
    tiles[i] = tiles[i-1];
  }

  tiles[0] = t;
}


std::mutex loadmutex;

void load_tile(int tx, int ty, int layer)
{
  printf("loading tile %d;%d, layer: %d\n",tx,ty, layer);

  struct heif_error err;

#if 0
  heif_item_id tile_id = heif_image_handle_get_image_tile_id(handle, tx, ty);

  heif_image_handle* tile_handle;
  err = heif_context_get_image_handle(ctx, tile_id, &tile_handle);
  if (err.code) {
    printf("heif_context_get_image_handle: %s\n", err.message);
    exit(0);
  }
    
  heif_image* img;
  err = heif_decode_image(tile_handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
  if (err.code) {
    printf("heif_decode_image\n");
    exit(0);
  }
#else

  std::lock_guard<std::mutex> lock(loadmutex);
  
  heif_image* img;
  int x0 = tx; // * tw;
  int y0 = ty; // * th;

  heif_decoding_options* options = heif_decoding_options_alloc();
  options->ignore_transformations = !process_transformations;
  
  err = heif_image_handle_decode_image_tile(pymd_layer_handles[layer], &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options, x0,y0);
  heif_decoding_options_free(options);

  if (err.code) {
    printf("heif_decode_image error: %s\n", err.message);
    exit(0);
  }
#endif
  
  int stride;
  const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

  Color *pixels = (Color *)malloc(tw*th*sizeof(Color));

  // Fill the image with RGB pixels
  for (int y = 0; y < th; y++) {
    memcpy(&pixels[y*tw], data+y*stride, tw*4);
  }
  
  Image image = {
    .data = pixels,
    .width = tw,
    .height = th,
    .mipmaps = 1,
    .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
  };

  tilemutex.lock();
  for (size_t i=0;i<tiles.size();i++) {
    if (tiles[i].x == tx && tiles[i].y == ty && tiles[i].layer == layer) {
      tiles[i].state = tile_state::waiting_for_texture_load;
      tiles[i].image = image;
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

  printf("loading ...\n");

  ctx = heif_context_alloc();
  heif_context_read_from_file(ctx, input_filename, nullptr);

  printf("loading finished\n");
  
  // get a handle to the primary image
  heif_image_handle* handle;
  heif_context_get_primary_image_handle(ctx, &handle);
  //heif_context_get_image_handle(ctx, 7, &handle);
  heif_item_id primary_id = heif_image_handle_get_item_id(handle);
  heif_image_handle_release(handle);

  /*
struct heif_entity_group
{
  heif_entity_group_id entity_group_id;
  uint32_t entity_group_type;
  heif_item_id* entities;
  uint32_t num_entities;
};
  */

  int nGroups;
  struct heif_entity_group* groups = heif_context_get_entity_groups(ctx, heif_fourcc('p','y','m','d'), primary_id, &nGroups);
  std::cout << "nGroups: " << nGroups << "\n";
  if (nGroups>0) {
    assert(nGroups==1);
    pymd_layers.resize(groups[0].num_entities);
    pymd_layer_handles.resize(groups[0].num_entities);
    for (uint32_t i=0;i<groups[0].num_entities;i++) {
      pymd_layers[i] = groups[0].entities[i];
      heif_context_get_image_handle(ctx, pymd_layers[i], &pymd_layer_handles[i]);
      if (pymd_layers[i] == primary_id) {
	active_layer = i;
      }
    }
  }
  else {
    pymd_layers.push_back(primary_id);
    pymd_layer_handles.resize(1);
    heif_context_get_image_handle(ctx, primary_id, &pymd_layer_handles[0]);
    active_layer = 0;
  }
  heif_entity_groups_release(groups, nGroups);

  heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);

  tw = tiling.tile_width;
  th = tiling.tile_height;

  printf("tilesize: %u x %u\n",tiling.tile_width, tiling.tile_height);
  printf("tiles: %lu x %lu\n",tiling.num_columns, tiling.num_rows);


  InitWindow(window_width, window_height, "libheif-viewer");
  int x00=0, y00=0;
  int mx=0, my=0;
  int dx=0, dy=0;
  bool mouse_pressed=false;
  
  SetTargetFPS(50);
  
  while (!WindowShouldClose()) {

    BeginDrawing();
    ClearBackground({0,0,0,255});

    tilemutex.lock();

    int wheel = GetMouseWheelMove();  // 0, 1, -1
    //std::cout << "wheel: " << wheel << "\n";

    if (wheel==1 && active_layer < pymd_layers.size()-1) {
      std::cout << "zoom in\n";
      active_layer++;
      
      heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);

      int m_x = GetMouseX();
      int m_y = GetMouseY();

      x00 = (x00 + m_x)*2 - m_x;
      y00 = (y00 + m_y)*2 - m_y;
    }
    else if (wheel==-1 && active_layer > 0) {
      std::cout << "zoom out\n";
      active_layer--;

      heif_image_handle_get_image_tiling(pymd_layer_handles[active_layer], process_transformations, &tiling);

      int m_x = GetMouseX();
      int m_y = GetMouseY();

      x00 = (x00 + m_x)/2 - m_x;
      y00 = (y00 + m_y)/2 - m_y;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      mx = GetMouseX();
      my = GetMouseY();
      dx = dy = 0;
      mouse_pressed=true;
    }
    else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
      x00 -= dx;
      y00 -= dy;
      dx=dy=0;
      mouse_pressed=false;
    }
    else if (mouse_pressed) {
      dx = GetMouseX() - mx;
      dy = GetMouseY() - my;
    }

    int x0=x00 - dx;
    int y0=y00 - dy;

    int tile_idx_x0 = x0/tw;
    int tile_idx_y0 = y0/th;
    
    for (int ty=tile_idx_y0;ty*th-y0<window_height;ty++) {
      for (int tx=tile_idx_x0;tx*tw-x0<window_width;tx++) {

	if (tx<0 || tx>=tiling.num_columns)
	  continue;

	if (ty<0 || ty>=tiling.num_rows)
	  continue;

	bool tile_found = false;
	
	for (size_t i=0;i<tiles.size();i++) {
	  if (tiles[i].x == tx && tiles[i].y == ty && tiles[i].layer == active_layer) {
	    tile_found = true;
	    if (tiles[i].state == tile_state::ready) {
	      DrawTexture(tiles[i].texture, tx*tw-x0,ty*th-y0, WHITE);
	      move_to_front(i);
	    }
	    else if (tiles[i].state == tile_state::waiting_for_texture_load) {
	      tiles[i].texture = LoadTextureFromImage(tiles[i].image);
	      UnloadImage(tiles[i].image);
	      tiles[i].state = tile_state::ready;
	      DrawTexture(tiles[i].texture, tx*tw-x0,ty*th-y0, WHITE);
	      move_to_front(i);
	    }
	  }

	  DrawRectangleLines(tx*tw - x0, ty*th-y0, tw,th, WHITE);
	}

	if (!tile_found) {
	  if (tiles.size() == max_tiles) {
	    if (tiles.back().state == tile_state::ready) {
	      UnloadTexture(tiles.back().texture);
	    }
	    if (tiles.back().state == tile_state::waiting_for_texture_load) {
	      UnloadImage(tiles.back().image);
	    }

	    tiles.pop_back();
	  }

	  tile t;
	  t.x = tx;
	  t.y = ty;
	  t.layer = active_layer;
	  tiles.push_back(t);
	  move_to_front(tiles.size()-1);

	  // load tile
	  std::thread th(load_tile, tx, ty, active_layer);
	  th.detach();
	}
      }
    }
    tilemutex.unlock();
  
    EndDrawing();

    x0+=2;
    y0++;
  }

  while (!GetKeyPressed()) { sleep(1); }

  CloseWindow();
  
  heif_context_free(ctx);

  return 0;
}
