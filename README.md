# Tiled HEIF Image Viewer
Viewer for tiled HEIF images.

![image](https://github.com/user-attachments/assets/979c74af-b59c-4bec-8edd-16f8752c7b47)

Dependencies:
- libheif
- raylib

Pan with the mouse. If the image has a multi-resolution `pymd` pyramid group, you can use the mouse wheel to browse through the resolution layers.

## Example Images

| Content | Resolution | File Size | Description | Link | Notes |
| ------- | ---------- | --------- | ----------- | ---- | ----- |
| Rotterdam | 6656x7936 | 5.3 MB   | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/9oeXtYfPH44QBox) |   |
| Rotterdam | 6656x7936 | 22.5 MB   | `unci`/Brotli at highest resolution layer,<br>`tili`/AV1 at overview layers | [link](https://cloud.dirk-farin.de/s/rbqkSs4QWZFSneS) | *1 |
| Rio     | 64768x41216 | 161 MB   | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/KT6qgfmpKRTWep6)  |   |
| Mandelbrot B | 65536x65536 | 304 MB | `grid`/HEVC pyramid | [link](https://cloud.dirk-farin.de/s/3XzFPxfw9GPNrxK) |   |
| Mandelbrot C | 262144x261120 | 1.6 GB | `tili`/AV1 pyramid | [link](https://cloud.dirk-farin.de/s/iJ8PP8tYQfCY34r) |   |

Notes:
- *1: these images are not up-to-date with the latest specification version. They store the tile properties directly in the `tili` image item instead of children of `tilC`.

The aerial images were generated from the [SpaceNet datasets](https://spacenet.ai/).
