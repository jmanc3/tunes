# Drawing API and backends

`drawing::Context` is the backend-neutral immediate-mode API. It includes paths,
clips, transforms, colors, strokes, compositing groups, images, text measurement
and rendering, rounded rectangles, and shadows. `RawWindow::drawing_context` is a
borrowed pointer; do not retain it across window destruction or buffer resizing.

## Backend selection

Tunes uses OpenGL ES 3 through native Wayland/EGL by default. If EGL or ES3 cannot
be initialized, it reports the reason and falls back to Cairo. To explicitly use
software rendering:

```sh
TUNES_RENDERER=cairo ./build/tunes
```

`TUNES_RENDERER=opengl` also selects OpenGL, with the same automatic fallback.
Building requires the `egl`, `glesv2`, and `wayland-egl` pkg-config packages in
addition to the existing Cairo/Pango dependencies. Both CMake and Make include
the backends; CMake compiles them once into the `tunes_drawing` library.

## OpenGL renderer

- `src/drawing/opengl_context.cpp` implements the API. `gl_shaders.h` embeds the
  GLES 3 vertex/fragment shaders, so installed builds need no shader search path.
- Convex fills, images, and glyph quads batch into a streaming VBO. Complex fills
  use stencil winding/parity, and tessellated strokes use a stencil union to
  avoid dark seams where translucent segments overlap. Arcs adapt their polygon
  resolution to their device-space radius. Targets use up to 4x MSAA.
- Rectangle clips use scissoring plus shader coverage. Arbitrary clips use
  composable GPU masks. Opacity groups use GPU render targets; `push_group(Rect)`
  bounds a group before allocating its target. Artwork crossfades use this to
  avoid allocating and clearing the entire library viewport for each cover.
- Transient targets reuse 32-pixel size buckets. The idle target pool is limited
  to the larger of 64 MiB or two frame targets; live nested groups are retained
  until their drawing state releases them.
- Immutable artwork uploads once per cache residency. The image cache uses a
  lifetime witness to prevent stale texture reuse, discards dead images, and
  evicts least-recently-used entries over approximately 128 MiB at frame boundaries.
  Bilinear filtering uses linear sampling; `Good` uses trilinear mipmaps for
  downscaling. Completely clipped artwork is not uploaded.
- Album artwork uses one `CachedShadow` per library window. Cairo rasterizes a
  black rounded rectangle, its alpha receives a separable two-scale Gaussian
  blur (25% contact, 75% ambient at 2.5 times the contact sigma), and Cairo clears
  the original shape from the result. Kernel prefix sums integrate constant-alpha
  runs without sampling every blur tap at every pixel. The first draw uploads it
  as an ordinary cached image texture. Every
  matching cover reuses that texture; scrolling and pulse transforms do not
  regenerate it. Card dimensions, DPI, radius, blur, or offset changes replace
  the cached image. Style opacity and fade alpha multiply at draw time and never
  invalidate the full-opacity raster. Other shape shadows still use the generic drawing primitives.
- Pango/Cairo only shapes and rasterizes text on cache misses. Glyph coverage
  uploads as single-channel R8 textures; colors, alpha, transforms, and soft
  shadow copies are applied by the GPU. Measurement calls need no texture.
  Text caching is limited to 16 MiB and 2,048 layouts, with eviction at frame
  boundaries. Textures and layouts survive window resizing.
- `flush()` resolves directly from the multisample target to EGL framebuffer 0.
  The production path never reads a frame back to the CPU. `EGLWindow` owns the
  native window, EGL surface/context, and drawing resources, and makes the correct
  context current before drawing and destruction. Native resizing precedes EGL
  buffer acquisition. Wayland frame callbacks coalesce redraws while a frame is
  pending; idle windows do not continuously render.

GPU ownership is on the UI thread. `create_opengl_context(width, height)` requires
an already-current GLES 3 context and a single-sample default framebuffer.
`begin_frame()` starts a complete repaint; save/restore and groups must be balanced
between frames. GL objects must be destroyed while their owning context is
current. `OpenGLContext::stats()` exposes draw/upload/allocation counters and
estimated texture residency for tests or profiling. It performs no GPU readback.

The native presentation follows the [Khronos Wayland/EGL contract](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_platform_wayland.txt):
`eglSwapBuffers` attaches, damages, and commits the surface, so the OpenGL path
must not also attach Cairo's shared-memory buffers.

## Common semantics and Cairo

Coordinates and text constraints are pixels; font sizes are points, preserving
the existing typography. `text(..., false)` returns metrics without painting.
Image pixels are native-endian premultiplied `0xAARRGGBB` and must remain immutable
after publication; replace an `Image` to publish different pixels. Backends do
not retain a reference to the caller's pixel storage. Image drawing starts at the
origin and uses the current transform, filter, alpha, and composite operator.
`save`/`restore` does not include paths; filling, stroking, and clipping consume
paths. Popping a group restores the state at its push and makes the group its
paint source.

Cairo owns one context and Pango layout cache per shared-memory buffer slot. Its
mapped pixels outlive the context, and `flush()` runs before presentation. The
same UI drawing calls work with either backend.

## Artwork prefetch

Visible covers request previews first, then display-sized sharp images after the
initial frame. `AlbumArtPrefetch` advances through the rest of the library one
album at a time only when visible detail is ready and outstanding work has
finished. Neighboring offscreen rows no longer enqueue extraction ahead of visible
requests. Scrolling can submit foreground requests immediately; an already-running
background decode finishes, but no more background work starts until foreground
loading drains. Changing card size/DPI revisits the cursor for quality upgrades;
replacing the library resets its candidate list.

Prefetch prepares immutable CPU images. GPU upload and the preview-to-sharp fade
happen when a cover is drawn, keeping offscreen work out of the GL context and
preventing invisible albums from advancing their initial transition.
Each ready visible cover starts its fade independently of other covers and the
playback artwork. Only background prefetch waits for the foreground set. Library
shadows fade with the first sharp cover and remain at full configured opacity
after that initial transition.

## Verification

- `album_art_cache`: cache loading and deterministic foreground/prefetch ordering
  with blocked extraction, background completion, and size upgrades.
- `drawing`: Cairo offscreen API tests.
- `opengl_drawing`: surfaceless EGL pixel comparisons against Cairo, including
  paths, clip masks, reflected strokes, additive/nested/bounded groups, source and
  clear compositing, text/shadows, resizing, batching, and texture lifetime/cache
  reuse, and shared Cairo-generated artwork shadows. It skips if no surfaceless
  ES3 driver is available.
- `egl_window`: an isolated in-process Wayland compositor receives actual EGL
  swaps and verifies pixels, resized buffer dimensions, multiple contexts, and
  cleanup. It uses Mesa software rendering and needs local Unix IPC permissions.

```sh
cmake --build build -j4
ctest --test-dir build --output-on-failure
```
