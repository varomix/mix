#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>

// Assume an OpenGL loader is used (e.g., GLAD)
// #include <glad/glad.h> 
// If not using a loader, you'd use SDL_GL_GetProcAddress

int main(int argc, char* argv[]) {
    SDL_Window* window = NULL;
    SDL_GLContext gl_context = NULL;
    int status = 0;

    // 1. Initialize SDL
    if (!SDL_Init(SDL_INIT_VIDEO)) { // SDL3's SDL_Init returns a boolean
        fprintf(stderr, "Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    // 2. Set OpenGL attributes (request a modern OpenGL core profile)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Required on macOS
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // 3. Create a window with the OpenGL flag
    window = SDL_CreateWindow("SDL3 OpenGL C Example", 800, 600, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        goto cleanup;
    }

    // 4. Create the OpenGL context and make it current
    gl_context = SDL_GL_CreateContext(window);
    if (gl_context == NULL) {
        fprintf(stderr, "Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        goto cleanup;
    }

    // 5. Initialize an OpenGL loader if used (e.g., gladLoadGL())
    // if (!gladLoadGL()) { fprintf(stderr, "Failed to initialize GLAD\n"); goto cleanup; }

    // Enable vsync
    SDL_GL_SetSwapInterval(1);

    // 6. Main loop
    while (status == 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    status = 1;
                    break;
                case SDL_EVENT_KEY_UP:
                    if (event.key.key == SDLK_ESCAPE) {
                        status = 1;
                    }
                    break;
                // Handle other events (resize, input, etc.)
            }
        }

        // OpenGL rendering commands
        // Set the background color (e.g., a reddish color)
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f); 
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Your drawing code goes here...

        // Swap the buffers to show the rendered content
        SDL_GL_SwapWindow(window);
    }

cleanup:
    // 7. Cleanup
    if (gl_context) {
        SDL_GL_DestroyContext(gl_context);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    return status;
}
