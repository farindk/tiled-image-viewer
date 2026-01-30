# Tiled HEIF Image Viewer
Viewer for tiled HEIF images.

![image](https://github.com/user-attachments/assets/979c74af-b59c-4bec-8edd-16f8752c7b47)

Dependencies:
- [libheif](https://github.com/strukturag/libheif)
- [raylib](https://www.raylib.com/)

Pan with the mouse. If the image has a multi-resolution `pymd` pyramid group, you can use the mouse wheel to browse through the resolution layers.

## Usage

```
tiled_image_viewer [options] <image.heif | URL>
```

**Options:**
| Option | Description |
|--------|-------------|
| `-t, --trivial-reader` | Use trivial cache reader instead of block cache (URL mode) |
| `--no-transforms` | Do not process HEIF image transformations |
| `-u, --url` | Treat input as HTTP/HTTPS URL |
| `-p, --primary` | Start with primary image (default: start at overview image) |
| `-b, --block-size <kB>` | Block size in kB for block cache reader (default: 64) |
| `-h, --help` | Show help |

**Examples:**
```bash
# View local file
./tiled_image_viewer image.heif

# View remote image via URL (uses 64KB block cache by default)
./tiled_image_viewer -u https://example.com/image.heif

# View remote image with larger block cache
./tiled_image_viewer -u -b 128 https://example.com/image.heif

# View remote image with trivial cache reader
./tiled_image_viewer -u -t https://example.com/image.heif
```

## Example Images

| Content | Resolution | File Size | Description | Link | Notes |
| ------- | ---------- | --------- | ----------- | ---- | ----- |
| Rotterdam | 6656x7936 | 5.3 MB   | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/9oeXtYfPH44QBox) | *2 |
| Rotterdam | 6656x7936 | 22.5 MB   | `unci`/Brotli at highest resolution layer,<br>`tili`/AV1 at overview layers | [link](https://cloud.dirk-farin.de/s/rbqkSs4QWZFSneS) | *1 *2 |
| Rio     | 64768x41216 | 161 MB   | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/KT6qgfmpKRTWep6)  | *2 |
| Mandelbrot B | 65536x65536 | 304 MB | `grid`/HEVC pyramid | [link](https://cloud.dirk-farin.de/s/3XzFPxfw9GPNrxK) |   |
| Mandelbrot C | 262144x261120 | 1.6 GB | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/iJ8PP8tYQfCY34r) |   |
| M31 Andromeda Galaxy | 42208x9870 | 106.1 MB | `grid`/HEVC pyramid | [link](https://cloud.dirk-farin.de/s/9FtHJy9SmsNRWsn) | *3 |

Notes:
- *1: these images are not up-to-date with the latest specification version. They store the tile properties directly in the `tili` image item instead of children of `tilC`.
- *2: the aerial images were generated from the [SpaceNet datasets](https://spacenet.ai/).
- *3: the M31 Andromeda galaxy image was taken from the [NASA Hubble mission](https://science.nasa.gov/mission/hubble/science/explore-the-night-sky/hubble-messier-catalog/messier-31/).
