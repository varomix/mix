// C header for the 3D cube example.
// MIX binds to these functions via `use c "cube.h"`.

// Initialise GL context, compile shaders, create buffers.
void gfx_init(int width, int height);

// Start the animation loop (registers Emscripten frame callback).
void gfx_start_loop(void);

// Set the rotation angles for the next frame.
void gfx_set_rotation(float x, float y, float z);

// Render one frame (called from C event loop after update).
void gfx_render(void);

// Per-frame rotation state — simple get/set so MIX can read & increment.
float get_rx(void);
float get_ry(void);
float get_rz(void);
void  set_rx(float v);
void  set_ry(float v);
void  set_rz(float v);
