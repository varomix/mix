// GPU helper - handles complex struct setup for SDL3 GPU API
#include <SDL3/SDL.h>

// Create GPU device and claim window
SDL_GPUDevice *gpu_create_device(SDL_Window *window);

// Load a pre-compiled MSL shader from file
SDL_GPUShader *gpu_load_shader(SDL_GPUDevice *device, const char *path,
                               SDL_GPUShaderStage stage, int num_uniform_buffers);

// Create the triangle graphics pipeline
SDL_GPUGraphicsPipeline *gpu_create_triangle_pipeline(
    SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUShader *frag,
    SDL_GPUTextureFormat swapchain_format);

// Create a vertex buffer with float data
SDL_GPUBuffer *gpu_create_vertex_buffer(SDL_GPUDevice *device,
                                         const float *data, int float_count);

// Draw a frame: acquires swapchain, clears, binds pipeline+vbuf, draws, submits
// Returns false if swapchain not available
int gpu_draw_frame(SDL_GPUDevice *device, SDL_Window *window,
                   SDL_GPUGraphicsPipeline *pipeline, SDL_GPUBuffer *vbuf,
                   float angle);
