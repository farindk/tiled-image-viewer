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

#ifndef TILE_LOADER_H
#define TILE_LOADER_H

#include <cstdint>
#include <functional>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <vector>

struct TileRequest {
    int x, y;
    uint32_t layer;
    bool wanted;  // true if tile is currently visible
};

class TileLoader {
public:
    // Callback signature: void(int x, int y, uint32_t layer)
    using LoadCallback = std::function<void(int, int, uint32_t)>;

    TileLoader(LoadCallback callback, int num_workers = 1);
    ~TileLoader();

    // Non-copyable
    TileLoader(const TileLoader&) = delete;
    TileLoader& operator=(const TileLoader&) = delete;

    // Queue a tile for loading. If already queued, marks it as wanted.
    void queue_tile(int x, int y, uint32_t layer);

    // Mark all queued tiles as not wanted (call before each display pass)
    void mark_all_unwanted();

    // Stop all workers and clear queue
    void shutdown();

private:
    void worker_thread();
    TileRequest* pick_best_tile();  // Returns tile to load (wanted first, then LIFO)

    LoadCallback m_callback;
    std::deque<TileRequest> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_shutdown{false};
};

#endif
