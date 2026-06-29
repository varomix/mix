#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- the update function is defined by the MIX program ----
extern void update(void);

// ---- rotation state (read/written by MIX via get/set) ----
static float rx, ry, rz;

float get_rx(void) { return rx; }
float get_ry(void) { return ry; }
float get_rz(void) { return rz; }
void  set_rx(float v) { rx = v; }
void  set_ry(float v) { ry = v; }
void  set_rz(float v) { rz = v; }
void  gfx_set_rotation(float x, float y, float z) { rx = x; ry = y; rz = z; }

// ---- JS-controllable parameters (exported to the HTML layer) ----
static float rot_speed = 1.0f;
static float cube_size  = 1.0f;

float EMSCRIPTEN_KEEPALIVE get_rot_speed(void) { return rot_speed; }
void  EMSCRIPTEN_KEEPALIVE set_rot_speed(float s) { rot_speed = s; }
float EMSCRIPTEN_KEEPALIVE get_cube_size(void)  { return cube_size; }
void  EMSCRIPTEN_KEEPALIVE set_cube_size(float s)  { cube_size = s; }

// ---- GL resources ----
static GLuint shader_prog;
static GLuint vbo, ibo;
static int    location_mvp;

// ---- mat4 helpers (column-major, like GL) ----
typedef float mat4[16];

static void mat4_identity(mat4 m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_perspective(mat4 m, float fov_y, float aspect, float near, float far) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fov_y * 0.5f);
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (far + near) / (near - far);
    m[11] = -1.0f;
    m[14] = (2.0f * far * near) / (near - far);
}

static void mat4_multiply(mat4 r, const mat4 a, const mat4 b) {
    mat4 t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + i] * b[j * 4 + k];
            t[j * 4 + i] = sum;
        }
    memcpy(r, t, sizeof(t));
}

static void mat4_rotate_x(mat4 m, float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4 t; memcpy(t, m, sizeof(t));
    for (int i = 0; i < 4; i++) {
        float row1 = t[1 * 4 + i], row2 = t[2 * 4 + i];
        m[1 * 4 + i] = row1 * c + row2 * s;
        m[2 * 4 + i] = row1 * -s + row2 * c;
    }
}

static void mat4_rotate_y(mat4 m, float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4 t; memcpy(t, m, sizeof(t));
    for (int i = 0; i < 4; i++) {
        float row0 = t[0 * 4 + i], row2 = t[2 * 4 + i];
        m[0 * 4 + i] = row0 * c + row2 * -s;
        m[2 * 4 + i] = row0 * s + row2 * c;
    }
}

static void mat4_rotate_z(mat4 m, float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4 t; memcpy(t, m, sizeof(t));
    for (int i = 0; i < 4; i++) {
        float row0 = t[0 * 4 + i], row1 = t[1 * 4 + i];
        m[0 * 4 + i] = row0 * c + row1 * s;
        m[1 * 4 + i] = row0 * -s + row1 * c;
    }
}

static void mat4_translate(mat4 m, float x, float y, float z) {
    mat4 t; memcpy(t, m, sizeof(t));
    for (int i = 0; i < 4; i++)
        m[3 * 4 + i] = t[0 * 4 + i] * x + t[1 * 4 + i] * y + t[2 * 4 + i] * z + t[3 * 4 + i];
}

static void mat4_scale(mat4 m, float s) {
    for (int i = 0; i < 4; i++) {
        m[0 * 4 + i] *= s;
        m[1 * 4 + i] *= s;
        m[2 * 4 + i] *= s;
    }
}

// ---- shader helpers ----
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader error: %s\n", log);
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "link error: %s\n", log);
    }
    return p;
}

static const char *vert_src =
    "attribute vec3 aPos;\n"
    "attribute vec3 aColor;\n"
    "uniform mat4  uMVP;\n"
    "varying vec3  vColor;\n"
    "void main() {\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "  vColor = aColor;\n"
    "}\n";

static const char *frag_src =
    "precision mediump float;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(vColor, 1.0);\n"
    "}\n";

// ---- cube geometry ----
static const float verts[] = {
    -0.5f, -0.5f, -0.5f,    1,0,0,   0.5f,-0.5f,-0.5f,   1,0,0,
     0.5f, 0.5f,-0.5f,    1,0,0,  -0.5f, 0.5f,-0.5f,   1,0,0,
    -0.5f,-0.5f, 0.5f,    0,1,0,   0.5f,-0.5f, 0.5f,   0,1,0,
     0.5f, 0.5f, 0.5f,    0,1,0,  -0.5f, 0.5f, 0.5f,   0,1,0,
    -0.5f, 0.5f, 0.5f,    0,0,1,  -0.5f, 0.5f,-0.5f,   0,0,1,
     0.5f, 0.5f, 0.5f,    0,0,1,   0.5f, 0.5f,-0.5f,   0,0,1,
    -0.5f,-0.5f, 0.5f,    1,1,0,  -0.5f,-0.5f,-0.5f,   1,1,0,
     0.5f,-0.5f, 0.5f,    1,1,0,   0.5f,-0.5f,-0.5f,   1,1,0,
    -0.5f,-0.5f, 0.5f,    1,0,1,  -0.5f, 0.5f, 0.5f,   1,0,1,
    -0.5f,-0.5f,-0.5f,    1,0,1,  -0.5f, 0.5f,-0.5f,   1,0,1,
     0.5f,-0.5f, 0.5f,    0,1,1,   0.5f, 0.5f, 0.5f,   0,1,1,
     0.5f,-0.5f,-0.5f,    0,1,1,   0.5f, 0.5f,-0.5f,   0,1,1,
};

static const unsigned short indices[] = {
     0,1,2, 0,2,3,  4,6,5, 4,7,6,
     8,10,9, 9,10,11,  12,13,14, 13,15,14,
     16,17,18, 17,19,18,  20,21,22, 21,23,22,
};

// ---- initialisation ----
void gfx_init(int width, int height) {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attr);
    emscripten_webgl_make_context_current(ctx);

    emscripten_set_canvas_element_size("#canvas", width, height);
    glViewport(0, 0, width, height);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    shader_prog = link_program(vs, fs);
    glUseProgram(shader_prog);

    location_mvp = glGetUniformLocation(shader_prog, "uMVP");

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    GLint a_pos   = glGetAttribLocation(shader_prog, "aPos");
    GLint a_color = glGetAttribLocation(shader_prog, "aColor");
    glVertexAttribPointer(a_pos, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(a_pos);
    glVertexAttribPointer(a_color, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(a_color);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
    glEnable(GL_DEPTH_TEST);
}

// ---- render one frame ----
void gfx_render(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mat4 model, view, proj, mvp;
    mat4_identity(model);
    mat4_translate(model, 0, 0, -3.0f);
    mat4_rotate_x(model, rx);
    mat4_rotate_y(model, ry);
    mat4_rotate_z(model, rz);
    mat4_scale(model, cube_size);

    mat4_identity(view);
    mat4_perspective(proj, 3.14159f * 0.25f, 800.0f / 600.0f, 0.1f, 100.0f);
    mat4_multiply(mvp, proj, view);
    mat4_multiply(mvp, mvp, model);

    glUniformMatrix4fv(location_mvp, 1, GL_FALSE, mvp);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
}

// ---- frame callback ----
static void frame_callback(void) {
    update();
    gfx_render();
}

// ---- start the animation loop (called from MIX) ----
void gfx_start_loop(void) {
    emscripten_set_main_loop(frame_callback, 0, 0);
}
