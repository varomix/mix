#include "gpu_helper.h"
#include <stdio.h>
#include <string.h>

SDL_GPUDevice *gpu_create_device(SDL_Window *window) {
    SDL_GPUDevice *device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!device) {
        fprintf(stderr, "gpu: SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return NULL;
    }
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        fprintf(stderr, "gpu: SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        return NULL;
    }
    return device;
}

SDL_GPUShader *gpu_load_shader(SDL_GPUDevice *device, const char *path,
                               SDL_GPUShaderStage stage, int num_uniform_buffers) {
    size_t code_size;
    void *code = SDL_LoadFile(path, &code_size);
    if (!code) {
        fprintf(stderr, "gpu: failed to load shader '%s': %s\n", path, SDL_GetError());
        return NULL;
    }
    SDL_GPUShaderCreateInfo info = {0};
    info.code = (const Uint8 *)code;
    info.code_size = code_size;
    info.entrypoint = "main0";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_uniform_buffers = num_uniform_buffers;

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    SDL_free(code);
    if (!shader) {
        fprintf(stderr, "gpu: SDL_CreateGPUShader failed: %s\n", SDL_GetError());
    }
    return shader;
}

SDL_GPUGraphicsPipeline *gpu_create_triangle_pipeline(
    SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUShader *frag,
    SDL_GPUTextureFormat swapchain_format) {

    SDL_GPUVertexAttribute attrs[2] = {0};
    attrs[0].location = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[0].offset = 0;
    attrs[0].buffer_slot = 0;
    attrs[1].location = 1;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[1].offset = 12;
    attrs[1].buffer_slot = 0;

    SDL_GPUVertexBufferDescription vbuf_desc = {0};
    vbuf_desc.slot = 0;
    vbuf_desc.pitch = 28; // 3 pos + 4 color = 7 floats * 4 bytes
    vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexInputState vertex_input = {0};
    vertex_input.vertex_buffer_descriptions = &vbuf_desc;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attrs;
    vertex_input.num_vertex_attributes = 2;

    SDL_GPUColorTargetDescription color_target = {0};
    color_target.format = swapchain_format;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {0};
    pipeline_info.vertex_shader = vert;
    pipeline_info.fragment_shader = frag;
    pipeline_info.vertex_input_state = vertex_input;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.target_info.num_color_targets = 1;
    pipeline_info.target_info.color_target_descriptions = &color_target;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
    if (!pipeline) {
        fprintf(stderr, "gpu: SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());
    }
    return pipeline;
}

SDL_GPUBuffer *gpu_create_vertex_buffer(SDL_GPUDevice *device,
                                         const float *data, int float_count) {
    Uint32 size = float_count * sizeof(float);

    SDL_GPUBufferCreateInfo buf_info = {0};
    buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buf_info.size = size;
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(device, &buf_info);
    if (!buffer) return NULL;

    SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size = size;
    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(device, &tbuf_info);

    void *mapped = SDL_MapGPUTransferBuffer(device, tbuf, false);
    SDL_memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, tbuf);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation src = {0};
    src.transfer_buffer = tbuf;
    SDL_GPUBufferRegion dst = {0};
    dst.buffer = buffer;
    dst.size = size;
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);

    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, tbuf);

    return buffer;
}

int gpu_draw_frame(SDL_GPUDevice *device, SDL_Window *window,
                   SDL_GPUGraphicsPipeline *pipeline, SDL_GPUBuffer *vbuf,
                   float angle) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) return 0;

    // Push rotation uniform
    float uniform[4] = { angle, 0.0f, 0.0f, 0.0f };
    SDL_PushGPUVertexUniformData(cmd, 0, uniform, sizeof(uniform));

    // Acquire swapchain
    SDL_GPUTexture *tex = NULL;
    Uint32 w, h;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, window, &tex, &w, &h) || !tex) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return 0;
    }

    // Render pass with clear
    SDL_GPUColorTargetInfo color_info = {0};
    color_info.texture = tex;
    color_info.load_op = SDL_GPU_LOADOP_CLEAR;
    color_info.store_op = SDL_GPU_STOREOP_STORE;
    color_info.clear_color.r = 0.15f;
    color_info.clear_color.g = 0.15f;
    color_info.clear_color.b = 0.15f;
    color_info.clear_color.a = 1.0f;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_info, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, pipeline);

    SDL_GPUBufferBinding binding = {0};
    binding.buffer = vbuf;
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    SDL_SubmitGPUCommandBuffer(cmd);
    return 1;
}
