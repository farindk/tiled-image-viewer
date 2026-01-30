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

#include "tile_loader.h"
#include <algorithm>

TileLoader::TileLoader(LoadCallback callback, int num_workers)
    : m_callback(std::move(callback))
{
    for (int i = 0; i < num_workers; i++) {
        m_workers.emplace_back(&TileLoader::worker_thread, this);
    }
}

TileLoader::~TileLoader()
{
    shutdown();
}

void TileLoader::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
}

void TileLoader::queue_tile(int x, int y, uint32_t layer)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Check if already in queue
    for (auto& req : m_queue) {
        if (req.x == x && req.y == y && req.layer == layer) {
            req.wanted = true;
            return;
        }
    }

    // Add new request at front (LIFO for new tiles)
    m_queue.push_front({x, y, layer, true});
    m_cv.notify_one();
}

void TileLoader::mark_all_unwanted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& req : m_queue) {
        req.wanted = false;
    }
}

TileRequest* TileLoader::pick_best_tile()
{
    // First: find a wanted tile (prioritize visible tiles)
    for (auto& req : m_queue) {
        if (req.wanted) {
            return &req;
        }
    }
    // Second: take front tile (most recently added unwanted)
    if (!m_queue.empty()) {
        return &m_queue.front();
    }
    return nullptr;
}

void TileLoader::worker_thread()
{
    while (true) {
        TileRequest req;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_shutdown || !m_queue.empty();
            });

            if (m_shutdown) {
                return;
            }

            TileRequest* best = pick_best_tile();
            if (!best) {
                continue;
            }

            req = *best;

            // Remove from queue
            m_queue.erase(
                std::find_if(m_queue.begin(), m_queue.end(),
                    [&req](const TileRequest& r) {
                        return r.x == req.x && r.y == req.y && r.layer == req.layer;
                    }));
        }

        // Execute callback outside the lock
        m_callback(req.x, req.y, req.layer);
    }
}
