/*
 * video_gles.c — GPU presentation of core frames through a shader preset, and the OpenGL ES
 * context for libretro hardware-rendered cores (SET_HW_RENDER).
 *
 * EGL lifecycle
 *   - rn_video_set_window(win): takes ownership of `win` (one ANativeWindow reference). Creates the
 *     EGLDisplay / config / context on first use (ES 3 context, ES 2 fallback), then a window
 *     surface, makes it current on the calling thread, eglSwapInterval(0).
 *   - rn_video_set_window(NULL): destroys the EGL window surface, releases the window. The context
 *     (and its program / texture / VBO) stays alive so the next attach is fast. Software cores:
 *     nothing is current afterwards. HW cores: the 1x1 pbuffer is made current instead, so the core
 *     keeps a current context and can keep rendering into its FBO.
 *   - Swap failure: EGL_CONTEXT_LOST tears down surface + context (window kept); EGL_BAD_SURFACE /
 *     EGL_BAD_NATIVE_WINDOW tears down the surface. rn_video_present() then returns false until the
 *     caller re-attaches with rn_video_set_window(window). (HW cores: see "Context loss" below.)
 *   - rn_video_shutdown() (bridge release_all): frees everything, eglTerminate.
 *
 * Hardware-rendered cores (SET_HW_RENDER, GLES 2 / GLES 3 / GLES 3.x)
 *   - rn_video_hw_set() runs inside retro_load_game: creates display/config (window + pbuffer
 *     capable) and the context right away, makes it current on a 1x1 pbuffer (or surfaceless), reads
 *     the real ES version and accepts or rejects the request. It fills get_current_framebuffer /
 *     get_proc_address. Desktop GL, GL core, Vulkan, D3D ... are rejected so the core can fall back.
 *   - rn_video_hw_start() runs after retro_load_game succeeded: context current (window surface if
 *     attached, else pbuffer), FBO = RGBA8 color texture (+ depth24 / depth24-stencil8 renderbuffer
 *     when asked) sized to geometry.max_width/max_height, then hw.context_reset().
 *   - The FBO NAME never changes while the context lives (cores such as glsm / PPSSPP cache the value
 *     of get_current_framebuffer()). Growth (SET_SYSTEM_AV_INFO / SET_GEOMETRY / a bigger frame) only
 *     reallocates the attachments' storage, deferred to rn_video_hw_begin_frame() (emulation thread,
 *     between retro_run calls) so environment calls from other core threads never touch GL.
 *   - Window attach reuses the same context (no reset). Window detach makes the pbuffer current.
 *   - Context loss (EGL_CONTEXT_LOST from eglMakeCurrent / eglSwapBuffers): flagged, then recovered
 *     at the next begin_frame / present / attach: context_destroy(), new context + objects + FBO,
 *     context_reset().
 *   - rn_video_hw_context_destroy() (rn_unload_game, before retro_unload_game, like RetroArch's
 *     video_driver_free_hw_context): context current, hw.context_destroy(). The context itself and
 *     the FBO stay alive until rn_video_shutdown() (after retro_deinit), so late GL calls from the
 *     core's unload/deinit still have a current context.
 *   - Frames: RETRO_HW_FRAME_BUFFER_VALID → rn_video_hw_on_frame(w, h); present samples the FBO color
 *     texture through the same preset / rotation pipeline. bottom_left_origin (GL convention, what
 *     both N64 and PSP cores use) is handled with a second quad whose v coordinate is flipped.
 *   - GL state hygiene: the core shares our context. Every present binds / sets everything the
 *     presenter depends on (FBO 0, own VAO on ES 3, VBO + attrib pointers, program, texture unit 0 +
 *     sampler 0, viewport, depth/stencil/scissor/blend/cull/dither/... disabled, color mask, unpack
 *     buffer 0 for uploads). For HW cores the state touched is snapshotted first and restored after
 *     the swap, so caching state trackers in the core (glsm, GLideN64, PPSSPP's render manager)
 *     still match the real GL state on the next retro_run.
 *   - rn_video_hw_read_frame(): glReadPixels of the FBO (RGBA8) → top-down ARGB_8888 for thumbnails.
 *
 * Pixel formats (software frames are tightly packed by the bridge)
 *   - RGB565    → GL_RGB / GL_UNSIGNED_SHORT_5_6_5, uploaded directly (UNPACK_ALIGNMENT 2).
 *   - 0RGB1555  → converted on the CPU to RGB565 in a reusable buffer, then as above.
 *   - XRGB8888  → swizzled on the CPU (memory B,G,R,X → R,G,B,0xFF) into a reusable buffer and
 *                 uploaded as GL_RGBA / GL_UNSIGNED_BYTE (UNPACK_ALIGNMENT 4). Shaders always see
 *                 plain RGB; no swizzle uniform exists.
 *   The texture is reallocated (glTexImage2D) whenever the frame size or format changes, otherwise
 *   updated with glTexSubImage2D. NPOT + CLAMP_TO_EDGE + no mipmaps (valid on ES 2 and ES 3).
 *
 * Frame flag: present NEVER clears video_frame.frame_ready (nativeGetFrameBuffer still consumes it
 * for thumbnails). The bridge calls rn_video_on_frame() per new frame; present uploads only when
 * that sequence differs from the last uploaded one, otherwise it redraws the existing texture.
 *
 * Owned by native-bridge-agent.
 */
#include "video_gles.h"
#include "retronest_bridge.h"
#include "libretro.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>   /* ES 3.0 API; also every ES 2.0 entry point used by the ES 2 fallback */
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VLOG_TAG "RetroNest-Video"
#define VLOGI(...) __android_log_print(ANDROID_LOG_INFO,  VLOG_TAG, __VA_ARGS__)
#define VLOGW(...) __android_log_print(ANDROID_LOG_WARN,  VLOG_TAG, __VA_ARGS__)
#define VLOGE(...) __android_log_print(ANDROID_LOG_ERROR, VLOG_TAG, __VA_ARGS__)

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

#define ATTRIB_POSITION 0
#define ATTRIB_TEXCOORD 1

/* Fallback FBO size when the core reports no geometry, and a sanity cap. */
#define HW_DEFAULT_W 640
#define HW_DEFAULT_H 480

/* ------------------------------------------------------------------ */
/* Built-in passthrough (identical to assets/shaders/passthrough.*)    */
/* ------------------------------------------------------------------ */

static const char* const BUILTIN_VS =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "uniform mat4 u_mvp;\n"
    "uniform highp vec2 u_sourceSize;\n"
    "uniform highp vec2 u_textureSize;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    v_texCoord = a_texCoord * (u_sourceSize / u_textureSize);\n"
    "    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char* const BUILTIN_FS =
    "precision mediump float;\n"
    "uniform sampler2D u_texture;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(u_texture, v_texCoord).rgb, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint prog;
    GLint  u_mvp;
    GLint  u_texture;
    GLint  u_sourceSize;
    GLint  u_textureSize;
    GLint  u_outputSize;
} rn_program_t;

static EGLDisplay     g_display = EGL_NO_DISPLAY;
static EGLConfig      g_config  = NULL;
static EGLContext     g_context = EGL_NO_CONTEXT;
static EGLSurface     g_surface = EGL_NO_SURFACE;   /* window surface                       */
static EGLSurface     g_pbuffer = EGL_NO_SURFACE;   /* 1x1, HW cores without a window       */
static bool           g_pbuffer_tried = false;      /* creation attempted for this context  */
static bool           g_surfaceless = false;        /* EGL_KHR_surfaceless_context fallback */
static ANativeWindow* g_window  = NULL;
static int            g_es_major = 0;
static int            g_es_minor = 0;

static rn_program_t g_builtin;          /* passthrough, always present while a context exists  */
static rn_program_t g_user;             /* active preset (prog == 0 → use builtin)              */
static bool         g_user_linear = false;
static bool         g_gl_ready = false; /* builtin program / VBOs / texture exist in g_context  */

/* Sources of the active preset, kept to rebuild after context loss / deferred compile. */
static char* g_src_vs = NULL;
static char* g_src_fs = NULL;
static bool  g_src_linear = false;
static bool  g_src_dirty  = false;      /* stored while no context was current: compile on attach */

static GLuint   g_vbo = 0;              /* quad, v = 0 at the top of the screen                 */
static GLuint   g_vbo_flip = 0;         /* quad, v = 0 at the bottom (bottom-left-origin FBOs)  */
static GLuint   g_vao = 0;              /* ES 3 only: our own VAO, never the core's             */
static GLuint   g_texture = 0;
static unsigned g_tex_w = 0, g_tex_h = 0;
static int      g_tex_fmt = -1;         /* rn_pixel_format_t of the allocated texture, -1 none  */
static int      g_tex_filter = -1;      /* GL_NEAREST / GL_LINEAR currently set on the texture  */

static uint64_t g_frame_seq    = 0;     /* bumped by rn_video_on_frame()                        */
static uint64_t g_uploaded_seq = 0;
static bool     g_tex_valid    = false; /* texture holds a frame                                */

static uint8_t* g_conv_buf = NULL;
static size_t   g_conv_cap = 0;

static int g_rotation = 0;

/* Hardware rendering (one core, one context). */
typedef struct {
    struct retro_hw_render_callback cb; /* copy of what the core asked for (and we filled)       */
    bool     requested;   /* SET_HW_RENDER accepted for the game being loaded                     */
    bool     active;      /* context_reset() was called; frames come from the FBO                */
    bool     lost;        /* EGL_CONTEXT_LOST seen; recover before the next GL use               */
    bool     failed;      /* recovery / start failed: the core cannot run                        */
    GLuint   fbo, color_tex, depth_rb;
    unsigned fbo_w, fbo_h;     /* allocated storage                                              */
    unsigned want_w, want_h;   /* requested (max geometry / largest frame seen)                  */
    unsigned frame_w, frame_h; /* last RETRO_HW_FRAME_BUFFER_VALID frame (clamped to the FBO)    */
    bool     has_frame;
    int      tex_filter;
} rn_hw_t;

static rn_hw_t g_hw;
/* The context was created for / handed to a HW core in this session: keep it current (pbuffer)
 * whenever the window goes away, until rn_video_shutdown(). */
static bool g_hw_ctx = false;

static uint8_t* g_read_buf = NULL;      /* glReadPixels staging (RGBA)                           */
static size_t   g_read_cap = 0;
static uint32_t* g_argb_buf = NULL;     /* converted readback handed to JNI                      */
static size_t    g_argb_cap = 0;

static bool hw_recover(void);

/* ------------------------------------------------------------------ */
/* Shaders                                                             */
/* ------------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &n, log);
        VLOGE("%s shader compile failed: %.*s", type == GL_VERTEX_SHADER ? "Vertex" : "Fragment",
              (int)n, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool build_program(const char* vs_src, const char* fs_src, rn_program_t* out) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return false; }

    GLuint prog = glCreateProgram();
    if (!prog) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, ATTRIB_POSITION, "a_position");
    glBindAttribLocation(prog, ATTRIB_TEXCOORD, "a_texCoord");
    glLinkProgram(prog);
    glDeleteShader(vs); /* flagged; freed with the program */
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), &n, log);
        VLOGE("Shader link failed: %.*s", (int)n, log);
        glDeleteProgram(prog);
        return false;
    }

    out->prog          = prog;
    out->u_mvp         = glGetUniformLocation(prog, "u_mvp");
    out->u_texture     = glGetUniformLocation(prog, "u_texture");
    out->u_sourceSize  = glGetUniformLocation(prog, "u_sourceSize");
    out->u_textureSize = glGetUniformLocation(prog, "u_textureSize");
    out->u_outputSize  = glGetUniformLocation(prog, "u_outputSize");
    return true;
}

static void delete_program(rn_program_t* p) {
    if (p->prog) glDeleteProgram(p->prog);
    memset(p, 0, sizeof(*p));
}

static char* dup_str(const char* s) {
    const size_t n = strlen(s) + 1;
    char* c = malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

static void free_sources(void) {
    free(g_src_vs); g_src_vs = NULL;
    free(g_src_fs); g_src_fs = NULL;
    g_src_linear = false;
    g_src_dirty  = false;
}

/* Compiles the stored preset sources into g_user (context must be current). On failure the stored
 * sources are dropped and the builtin passthrough is used. */
static void compile_stored_sources(void) {
    g_src_dirty = false;
    if (!g_src_vs || !g_src_fs) return;
    rn_program_t p;
    memset(&p, 0, sizeof(p));
    if (build_program(g_src_vs, g_src_fs, &p)) {
        delete_program(&g_user);
        g_user = p;
        g_user_linear = g_src_linear;
        VLOGI("Shader compiled (linear=%s)", g_user_linear ? "true" : "false");
    } else {
        VLOGW("Stored shader preset failed to build; using built-in passthrough");
        delete_program(&g_user);
        g_user_linear = false;
        free_sources();
    }
}

/* ------------------------------------------------------------------ */
/* EGL                                                                 */
/* ------------------------------------------------------------------ */

static bool context_is_live(void) {
    return g_context != EGL_NO_CONTEXT && g_surface != EGL_NO_SURFACE;
}

static bool ensure_display(void) {
    if (g_display != EGL_NO_DISPLAY) return true;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY) {
        VLOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(d, NULL, NULL)) {
        VLOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    g_display = d;
    return true;
}

/* Picks an RGB888 config for `renderable` + `surface_type` (prefers no alpha, then 8-bit alpha). */
static bool choose_config(EGLint renderable, EGLint surface_type, EGLConfig* out) {
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, renderable,
        EGL_SURFACE_TYPE,    surface_type,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_NONE
    };
    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(g_display, attribs, configs, 64, &count) || count <= 0) return false;

    int best = -1, best_score = -1;
    for (EGLint i = 0; i < count; i++) {
        EGLint r = 0, g = 0, b = 0, a = 0, depth = 0;
        eglGetConfigAttrib(g_display, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(g_display, configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(g_display, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(g_display, configs[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(g_display, configs[i], EGL_DEPTH_SIZE, &depth);
        if (r != 8 || g != 8 || b != 8) continue;
        int score = 1;
        if (a == 0) score += 2; else if (a == 8) score += 1;
        if (depth == 0) score += 1;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    *out = configs[best >= 0 ? best : 0];
    return true;
}

static void log_config(EGLConfig cfg) {
    EGLint id = 0, r = 0, g = 0, b = 0, a = 0, depth = 0, stencil = 0, surf = 0;
    eglGetConfigAttrib(g_display, cfg, EGL_CONFIG_ID, &id);
    eglGetConfigAttrib(g_display, cfg, EGL_RED_SIZE, &r);
    eglGetConfigAttrib(g_display, cfg, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(g_display, cfg, EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(g_display, cfg, EGL_ALPHA_SIZE, &a);
    eglGetConfigAttrib(g_display, cfg, EGL_DEPTH_SIZE, &depth);
    eglGetConfigAttrib(g_display, cfg, EGL_STENCIL_SIZE, &stencil);
    eglGetConfigAttrib(g_display, cfg, EGL_SURFACE_TYPE, &surf);
    VLOGI("EGL config id=%d R%dG%dB%dA%d depth=%d stencil=%d surface_type=0x%x", id, r, g, b, a,
          depth, stencil, surf);
}

/* Creates the context if needed. `hw`: the config must also support pbuffers when possible (a HW
 * core needs a current context without a window). Software cores keep the original window-only
 * config selection. */
static bool ensure_context(bool hw) {
    if (g_context != EGL_NO_CONTEXT) return true;

    static const struct { EGLint bit; EGLint version; } tries[] = {
        { EGL_OPENGL_ES3_BIT, 3 },
        { EGL_OPENGL_ES2_BIT, 2 },
    };
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        EGLConfig cfg;
        bool have = false;
        if (hw) have = choose_config(tries[i].bit, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, &cfg);
        if (!have) have = choose_config(tries[i].bit, EGL_WINDOW_BIT, &cfg);
        if (!have) continue;
        const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, tries[i].version, EGL_NONE };
        EGLContext ctx = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT) {
            VLOGW("eglCreateContext (ES %d) failed: 0x%x", tries[i].version, eglGetError());
            continue;
        }
        g_config   = cfg;
        g_context  = ctx;
        g_es_major = tries[i].version;
        g_es_minor = 0;
        g_pbuffer_tried = false;
        g_surfaceless = false;
        return true;
    }
    VLOGE("No usable EGL config/context (ES 3 or ES 2)");
    return false;
}

static bool has_egl_extension(const char* name) {
    const char* exts = eglQueryString(g_display, EGL_EXTENSIONS);
    if (!exts) return false;
    const size_t n = strlen(name);
    for (const char* p = exts; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == exts || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    }
    return false;
}

static bool has_gl_extension(const char* name) {
    const char* exts = (const char*)glGetString(GL_EXTENSIONS);
    if (!exts) return false;
    const size_t n = strlen(name);
    for (const char* p = exts; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == exts || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    }
    return false;
}

/* Makes g_context current without a window: 1x1 pbuffer, else surfaceless. */
static bool make_headless_current(void) {
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT) return false;
    if (g_pbuffer == EGL_NO_SURFACE && !g_pbuffer_tried) {
        g_pbuffer_tried = true;
        const EGLint pb_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        g_pbuffer = eglCreatePbufferSurface(g_display, g_config, pb_attribs);
        if (g_pbuffer == EGL_NO_SURFACE) {
            VLOGW("eglCreatePbufferSurface failed: 0x%x", eglGetError());
            g_surfaceless = has_egl_extension("EGL_KHR_surfaceless_context");
            VLOGI("Surfaceless context %s", g_surfaceless ? "available" : "unavailable");
        }
    }
    if (g_pbuffer == EGL_NO_SURFACE && !g_surfaceless) return false;

    const EGLSurface s = g_pbuffer; /* EGL_NO_SURFACE when surfaceless */
    if (eglGetCurrentContext() == g_context && eglGetCurrentSurface(EGL_DRAW) == s) return true;
    if (!eglMakeCurrent(g_display, s, s, g_context)) {
        const EGLint err = eglGetError();
        VLOGE("eglMakeCurrent (headless) failed: 0x%x", err);
        if (err == EGL_CONTEXT_LOST && g_hw_ctx) g_hw.lost = true;
        return false;
    }
    return true;
}

static void reset_bindings(void) {
    if (g_es_major >= 3) glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

/* Reads the real ES version of the current context ("OpenGL ES 3.2 ..."). */
static void query_es_version(void) {
    const char* ver = (const char*)glGetString(GL_VERSION);
    int maj = 0, min = 0;
    if (ver && sscanf(ver, "OpenGL ES %d.%d", &maj, &min) == 2 && maj >= 2) {
        g_es_major = maj;
        g_es_minor = min;
    } else {
        g_es_minor = 0; /* keep the requested client version */
    }
}

/* Creates the GL objects of a freshly created context (it must be current). */
static bool init_gl_objects(void) {
    query_es_version();
    VLOGI("GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
    VLOGI("GL_RENDERER=%s", (const char*)glGetString(GL_RENDERER));
    VLOGI("OpenGL ES context version %d.%d", g_es_major, g_es_minor);
    log_config(g_config);

    memset(&g_builtin, 0, sizeof(g_builtin));
    memset(&g_user, 0, sizeof(g_user));
    if (!build_program(BUILTIN_VS, BUILTIN_FS, &g_builtin)) {
        VLOGE("Built-in passthrough program failed");
        return false;
    }

    /* Unit quad as a triangle strip: x, y, u, v. v = 0 is the top source row. */
    static const GLfloat quad[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    /* Same quad for bottom-left-origin textures (HW FBOs): v = 0 is the bottom screen row. */
    static const GLfloat quad_flip[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    if (g_es_major >= 3) {
        glGenVertexArrays(1, &g_vao);
        glBindVertexArray(g_vao);
    }
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenBuffers(1, &g_vbo_flip);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo_flip);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_flip), quad_flip, GL_STATIC_DRAW);

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &g_texture);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_tex_filter = GL_NEAREST;
    g_tex_w = g_tex_h = 0;
    g_tex_fmt = -1;
    g_tex_valid = false;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);

    compile_stored_sources();
    /* A HW core starts from a clean context: leave nothing of ours bound. */
    reset_bindings();
    g_gl_ready = true;
    return true;
}

static void forget_gl_objects(void) {
    memset(&g_builtin, 0, sizeof(g_builtin));
    memset(&g_user, 0, sizeof(g_user));
    g_user_linear = false;
    g_vbo = 0;
    g_vbo_flip = 0;
    g_vao = 0;
    g_texture = 0;
    g_tex_w = g_tex_h = 0;
    g_tex_fmt = -1;
    g_tex_filter = -1;
    g_tex_valid = false;
    g_gl_ready = false;
    /* HW FBO objects live in the context too. */
    g_hw.fbo = g_hw.color_tex = g_hw.depth_rb = 0;
    g_hw.fbo_w = g_hw.fbo_h = 0;
    g_hw.tex_filter = -1;
    g_hw.has_frame = false;
    if (g_src_vs) g_src_dirty = true; /* rebuild the preset in the next context */
}

/* Destroys the window surface. A HW core keeps its context current through the pbuffer. */
static void destroy_surface(void) {
    if (g_display == EGL_NO_DISPLAY) return;
    if (g_hw_ctx && g_context != EGL_NO_CONTEXT && !g_hw.lost) {
        if (!make_headless_current()) {
            eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    } else {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (g_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
    }
}

/* Makes g_context current on whatever surface exists (window first). Never creates surfaces. */
static bool make_any_current(void) {
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT) return false;
    if (g_surface != EGL_NO_SURFACE) return eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    if (g_pbuffer != EGL_NO_SURFACE || g_surfaceless) {
        return eglMakeCurrent(g_display, g_pbuffer, g_pbuffer, g_context);
    }
    return false;
}

/* Destroys the context. When `delete_objects` and a surface can be made current, GL objects are
 * deleted explicitly first (context destruction frees them anyway). */
static void destroy_context(bool delete_objects) {
    if (g_display == EGL_NO_DISPLAY || g_context == EGL_NO_CONTEXT) {
        forget_gl_objects();
        return;
    }
    if (delete_objects && make_any_current()) {
        delete_program(&g_user);
        delete_program(&g_builtin);
        if (g_vbo) glDeleteBuffers(1, &g_vbo);
        if (g_vbo_flip) glDeleteBuffers(1, &g_vbo_flip);
        if (g_vao && g_es_major >= 3) glDeleteVertexArrays(1, &g_vao);
        if (g_texture) glDeleteTextures(1, &g_texture);
        if (g_hw.fbo) glDeleteFramebuffers(1, &g_hw.fbo);
        if (g_hw.color_tex) glDeleteTextures(1, &g_hw.color_tex);
        if (g_hw.depth_rb) glDeleteRenderbuffers(1, &g_hw.depth_rb);
    }
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
    }
    if (g_pbuffer != EGL_NO_SURFACE) {
        eglDestroySurface(g_display, g_pbuffer);
        g_pbuffer = EGL_NO_SURFACE;
    }
    g_pbuffer_tried = false;
    g_surfaceless = false;
    eglDestroyContext(g_display, g_context);
    g_context = EGL_NO_CONTEXT;
    g_config  = NULL;
    g_es_major = 0;
    g_es_minor = 0;
    forget_gl_objects();
}

static void release_window(void) {
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = NULL;
    }
}

static bool make_current(void) {
    if (eglGetCurrentContext() == g_context && eglGetCurrentSurface(EGL_DRAW) == g_surface) {
        return true;
    }
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        const EGLint err = eglGetError();
        VLOGE("eglMakeCurrent failed: 0x%x", err);
        if (err == EGL_CONTEXT_LOST && g_hw_ctx) g_hw.lost = true;
        return false;
    }
    return true;
}

/* HW cores: the context current on this thread, on the window surface when there is one. */
static bool make_hw_current(void) {
    if (g_context == EGL_NO_CONTEXT) return false;
    if (g_surface != EGL_NO_SURFACE) return make_current();
    return make_headless_current();
}

/* Builds display/context/surface for g_window and makes it current. */
static bool attach_surface(void) {
    if (!ensure_display()) return false;
    if (!ensure_context(g_hw_ctx)) return false;

    EGLint visual = 0;
    if (eglGetConfigAttrib(g_display, g_config, EGL_NATIVE_VISUAL_ID, &visual) && visual != 0) {
        ANativeWindow_setBuffersGeometry(g_window, 0, 0, visual);
    }
    g_surface = eglCreateWindowSurface(g_display, g_config, g_window, NULL);
    if (g_surface == EGL_NO_SURFACE) {
        VLOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        const EGLint err = eglGetError();
        VLOGE("eglMakeCurrent failed: 0x%x", err);
        if (err == EGL_CONTEXT_LOST && g_hw_ctx) {
            g_hw.lost = true;   /* recovered by the caller / next begin_frame */
            destroy_surface();
        } else {
            destroy_surface();
            if (err == EGL_CONTEXT_LOST) destroy_context(false);
        }
        return false;
    }
    eglSwapInterval(g_display, 0); /* pacing is the engine's job; never block on vsync */

    if (!g_gl_ready) {
        if (!init_gl_objects()) {
            if (!g_hw_ctx) destroy_context(true);
            else destroy_surface();
            return false;
        }
    } else {
        g_tex_valid = g_tex_valid && g_texture != 0;
        if (g_src_dirty) compile_stored_sources();
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* GL state snapshot (HW cores share our context)                      */
/* ------------------------------------------------------------------ */

static const GLenum SAVED_CAPS[] = {
    GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_DITHER,
    GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE,
    GL_RASTERIZER_DISCARD, /* ES 3 only: last, skipped on ES 2 */
};
#define SAVED_CAP_COUNT (sizeof(SAVED_CAPS) / sizeof(SAVED_CAPS[0]))

typedef struct {
    GLint draw_fb, read_fb;   /* ES 2: draw_fb = FRAMEBUFFER_BINDING */
    GLint program, vao, array_buffer, unpack_buffer, active_texture, tex0, sampler0, renderbuffer;
    GLint viewport[4];
    GLboolean color_mask[4];
    GLfloat clear_color[4];
    GLboolean caps[SAVED_CAP_COUNT];
    /* ES 2 (no VAO): vertex attribs 0 / 1 are global state. */
    GLint attr_enabled[2], attr_size[2], attr_type[2], attr_norm[2], attr_stride[2], attr_buffer[2];
    void* attr_ptr[2];
} gl_saved_t;

static size_t cap_count(void) {
    return g_es_major >= 3 ? SAVED_CAP_COUNT : SAVED_CAP_COUNT - 1;
}

static void save_gl_state(gl_saved_t* s) {
    memset(s, 0, sizeof(*s));
    if (g_es_major >= 3) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fb);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpack_buffer);
    } else {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->draw_fb);
        for (GLuint i = 0; i < 2; i++) {
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr_enabled[i]);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s->attr_size[i]);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s->attr_type[i]);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s->attr_norm[i]);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s->attr_stride[i]);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s->attr_buffer[i]);
            glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s->attr_ptr[i]);
        }
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0);
    if (g_es_major >= 3) glGetIntegerv(GL_SAMPLER_BINDING, &s->sampler0);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    for (size_t i = 0; i < cap_count(); i++) s->caps[i] = glIsEnabled(SAVED_CAPS[i]);
}

static void restore_gl_state(const gl_saved_t* s) {
    if (g_es_major >= 3) {
        glBindVertexArray((GLuint)s->vao);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->unpack_buffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read_fb);
    } else {
        for (GLuint i = 0; i < 2; i++) {
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->attr_buffer[i]);
            if (s->attr_size[i] >= 1 && s->attr_size[i] <= 4) {
                glVertexAttribPointer(i, s->attr_size[i], (GLenum)s->attr_type[i],
                                      s->attr_norm[i] ? GL_TRUE : GL_FALSE, s->attr_stride[i],
                                      s->attr_ptr[i]);
            }
            if (s->attr_enabled[i]) glEnableVertexAttribArray(i);
            else glDisableVertexAttribArray(i);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->draw_fb);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->renderbuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex0);
    if (g_es_major >= 3) glBindSampler(0, (GLuint)s->sampler0);
    glActiveTexture((GLenum)s->active_texture);
    glUseProgram((GLuint)s->program);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    for (size_t i = 0; i < cap_count(); i++) {
        if (s->caps[i]) glEnable(SAVED_CAPS[i]);
        else glDisable(SAVED_CAPS[i]);
    }
}

/* Everything the presenter depends on, set unconditionally (the core may have left anything). */
static void set_present_state(EGLint sw, EGLint sh) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (g_es_major >= 3) {
        glBindVertexArray(g_vao);
        glBindSampler(0, 0);
    }
    for (size_t i = 0; i < cap_count(); i++) glDisable(SAVED_CAPS[i]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, sw, sh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Upload + draw                                                       */
/* ------------------------------------------------------------------ */

static uint8_t* conv_buffer(size_t bytes) {
    if (bytes > g_conv_cap) {
        uint8_t* grown = realloc(g_conv_buf, bytes);
        if (!grown) return NULL;
        g_conv_buf = grown;
        g_conv_cap = bytes;
    }
    return g_conv_buf;
}

static bool upload_frame(const rn_video_frame_t* f) {
    const unsigned w = f->width, h = f->height;
    const uint8_t* src = (const uint8_t*)f->data;
    const void* pixels = src;
    GLenum format, type;
    GLint align;

    switch (f->pixel_format) {
        case RN_PIXEL_FORMAT_XRGB8888: {
            uint32_t* out = (uint32_t*)conv_buffer((size_t)w * h * 4);
            if (!out) return false;
            for (unsigned y = 0; y < h; y++) {
                const uint32_t* row = (const uint32_t*)(src + (size_t)y * f->pitch);
                uint32_t* o = out + (size_t)y * w;
                for (unsigned x = 0; x < w; x++) {
                    const uint32_t p = row[x]; /* 0x00RRGGBB → memory R,G,B,0xFF (little-endian) */
                    o[x] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
                }
            }
            pixels = out;
            format = GL_RGBA; type = GL_UNSIGNED_BYTE; align = 4;
            break;
        }
        case RN_PIXEL_FORMAT_RGB565: {
            if (f->pitch != (size_t)w * 2) { /* bridge packs tightly; repack defensively */
                uint8_t* out = conv_buffer((size_t)w * h * 2);
                if (!out) return false;
                for (unsigned y = 0; y < h; y++) {
                    memcpy(out + (size_t)y * w * 2, src + (size_t)y * f->pitch, (size_t)w * 2);
                }
                pixels = out;
            }
            format = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; align = 2;
            break;
        }
        case RN_PIXEL_FORMAT_0RGB1555:
        default: {
            uint16_t* out = (uint16_t*)conv_buffer((size_t)w * h * 2);
            if (!out) return false;
            for (unsigned y = 0; y < h; y++) {
                const uint16_t* row = (const uint16_t*)(src + (size_t)y * f->pitch);
                uint16_t* o = out + (size_t)y * w;
                for (unsigned x = 0; x < w; x++) {
                    const uint16_t c = row[x];
                    const uint16_t g5 = (c >> 5) & 0x1F;
                    o[x] = (uint16_t)(((c & 0x7C00u) << 1) | (((g5 << 1) | (g5 >> 4)) << 5) |
                                      (c & 0x1Fu));
                }
            }
            pixels = out;
            format = GL_RGB; type = GL_UNSIGNED_SHORT_5_6_5; align = 2;
            break;
        }
    }

    const int fmt_key = f->pixel_format == RN_PIXEL_FORMAT_XRGB8888 ? 1 : 2; /* RGBA vs RGB565 */
    if (g_es_major >= 3) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); /* a bound PBO would turn `pixels` into an offset */
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    if (w != g_tex_w || h != g_tex_h || fmt_key != g_tex_fmt) {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, (GLsizei)w, (GLsizei)h, 0, format, type,
                     pixels);
        g_tex_w = w;
        g_tex_h = h;
        g_tex_fmt = fmt_key;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, format, type, pixels);
    }
    return true;
}

static float round_px(float v) { return (float)(int)(v + 0.5f); }

/* What to draw: a texture whose visible frame (src) sits at texture coordinate (0,0). */
typedef struct {
    GLuint   tex;
    unsigned src_w, src_h;   /* visible frame                                              */
    unsigned tex_w, tex_h;   /* allocated texture                                          */
    GLuint   vbo;            /* g_vbo (top-left origin) or g_vbo_flip (bottom-left origin) */
    int*     filter_state;   /* GL filter currently set on `tex`                           */
} draw_src_t;

static void draw(EGLint sw, EGLint sh, const draw_src_t* d) {
    const rn_program_t* p = g_user.prog ? &g_user : &g_builtin;
    const bool linear = g_user.prog ? g_user_linear : false;
    if (!p->prog || !d->tex || !d->src_w || !d->src_h || sw <= 0 || sh <= 0) return;

    /* Display aspect of the un-rotated image. */
    float aspect = rn_get_core_info()->aspect_ratio;
    if (!(aspect > 0.0f)) aspect = (float)d->src_w / (float)d->src_h;

    const int rot = g_rotation;
    const bool sideways = rot == 90 || rot == 270;
    const float box_aspect = sideways ? 1.0f / aspect : aspect;
    const float W = (float)sw, H = (float)sh;
    float bw, bh;
    if (W / H > box_aspect) { bh = H; bw = H * box_aspect; }
    else                    { bw = W; bh = W / box_aspect; }
    bw = round_px(bw); bh = round_px(bh);
    if (bw < 1.0f) bw = 1.0f;
    if (bh < 1.0f) bh = 1.0f;
    const float iw = sideways ? bh : bw; /* on-screen size along the image's own x / y axes */
    const float ih = sideways ? bw : bh;

    /* Clockwise rotation in NDC (y up): x' = x c + y s, y' = -x s + y c. */
    float c = 1.0f, s = 0.0f;
    if (rot == 90)       { c = 0.0f;  s = 1.0f;  }
    else if (rot == 180) { c = -1.0f; s = 0.0f;  }
    else if (rot == 270) { c = 0.0f;  s = -1.0f; }
    const GLfloat mvp[16] = { /* column-major */
         c * iw / W, -s * iw / H, 0.0f, 0.0f,
         s * ih / W,  c * ih / H, 0.0f, 0.0f,
         0.0f,        0.0f,       1.0f, 0.0f,
         0.0f,        0.0f,       0.0f, 1.0f,
    };

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex);
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    if (filter != *d->filter_state) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        *d->filter_state = filter;
    }

    glUseProgram(p->prog);
    if (p->u_texture >= 0)     glUniform1i(p->u_texture, 0);
    if (p->u_mvp >= 0)         glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp);
    if (p->u_sourceSize >= 0)  glUniform2f(p->u_sourceSize, (float)d->src_w, (float)d->src_h);
    if (p->u_textureSize >= 0) glUniform2f(p->u_textureSize, (float)d->tex_w, (float)d->tex_h);
    if (p->u_outputSize >= 0)  glUniform2f(p->u_outputSize, iw, ih);

    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    glEnableVertexAttribArray(ATTRIB_POSITION);
    glEnableVertexAttribArray(ATTRIB_TEXCOORD);
    glVertexAttribPointer(ATTRIB_POSITION, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)0);
    glVertexAttribPointer(ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Swaps; handles failures. Returns false when the frame could not be shown. */
static bool swap_or_handle_error(void) {
    if (eglSwapBuffers(g_display, g_surface)) return true;
    const EGLint err = eglGetError();
    VLOGW("eglSwapBuffers failed: 0x%x", err);
    if (err == EGL_CONTEXT_LOST) {
        if (g_hw_ctx) {
            g_hw.lost = true;     /* the HW core's context: recreate + context_reset */
        } else {
            destroy_context(false); /* objects died with the context */
        }
    } else if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW ||
               err == EGL_BAD_CURRENT_SURFACE) {
        destroy_surface();
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API (retronest_bridge.h, Phase 4C)                           */
/* ------------------------------------------------------------------ */

bool rn_video_set_window(struct ANativeWindow* window) {
    if (!window) {
        destroy_surface();
        release_window();
        return true;
    }

    if (window == g_window) {
        /* Re-attach of the same Surface (e.g. surfaceChanged after a rotation). We already hold a
         * reference, so drop the extra one; recreate the EGL window surface (context kept) so the
         * very next present uses the new buffer size. */
        ANativeWindow_release(window);
        destroy_surface();
    } else {
        destroy_surface();
        release_window();
        g_window = window;
    }

    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (g_hw.active && g_hw.lost) {
            /* hw_recover() re-attaches g_window itself. */
            ok = hw_recover() && context_is_live();
            if (ok) break;
            continue;
        }
        ok = attach_surface();
        if (!ok && !(g_hw.active && g_hw.lost)) break;
    }
    if (!ok) {
        destroy_surface();
        release_window();
        return false;
    }
    VLOGI("Window attached (%dx%d, ES %d.%d%s)", ANativeWindow_getWidth(g_window),
          ANativeWindow_getHeight(g_window), g_es_major, g_es_minor,
          g_hw.active ? ", HW core" : "");
    return true;
}

bool rn_video_set_shader(const char* vertex_src, const char* fragment_src, bool linear_filter) {
    if (!vertex_src || !fragment_src) return false;

    if (context_is_live() && g_gl_ready && make_current()) {
        rn_program_t p;
        memset(&p, 0, sizeof(p));
        if (!build_program(vertex_src, fragment_src, &p)) {
            return false; /* previous program (or builtin) stays active */
        }
        char* vs = dup_str(vertex_src);
        char* fs = dup_str(fragment_src);
        free_sources();
        if (vs && fs) {
            g_src_vs = vs; /* kept to rebuild after a context loss */
            g_src_fs = fs;
            g_src_linear = linear_filter;
        } else {
            free(vs);
            free(fs);
        }
        delete_program(&g_user);
        g_user = p;
        g_user_linear = linear_filter;
        VLOGI("Shader compiled (linear=%s)", linear_filter ? "true" : "false");
        return true;
    }

    /* No current context: store, compile when one appears. Validity is unknown until then. */
    char* vs = dup_str(vertex_src);
    char* fs = dup_str(fragment_src);
    if (!vs || !fs) { free(vs); free(fs); return false; }
    free_sources();
    g_src_vs = vs;
    g_src_fs = fs;
    g_src_linear = linear_filter;
    g_src_dirty = true;
    VLOGI("Shader stored; compiles when a surface is attached (linear=%s)",
          linear_filter ? "true" : "false");
    return true;
}

void rn_video_clear_shader(void) {
    /* Back to the built-in Sharp (passthrough) output: forget the stored sources and drop the preset program. */
    free_sources();
    if (g_user.prog) {
        if (context_is_live() && g_gl_ready && make_current()) {
            delete_program(&g_user);
        } else {
            memset(&g_user, 0, sizeof(g_user)); /* its context is gone / not current: the GL object went with it */
        }
    }
    g_user_linear = false;
    VLOGI("Shader cleared (built-in Sharp output)");
}

void rn_video_set_rotation(int degrees) {
    int d = degrees % 360;
    if (d < 0) d += 360;
    d = ((d + 45) / 90) * 90 % 360; /* snap to 0/90/180/270 */
    g_rotation = d;
}

static bool present_hw(void) {
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);

    gl_saved_t saved;
    save_gl_state(&saved);
    set_present_state(sw, sh);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g_hw.has_frame && g_hw.color_tex) {
        const draw_src_t d = {
            .tex = g_hw.color_tex,
            .src_w = g_hw.frame_w, .src_h = g_hw.frame_h,
            .tex_w = g_hw.fbo_w,   .tex_h = g_hw.fbo_h,
            .vbo = g_hw.cb.bottom_left_origin ? g_vbo_flip : g_vbo,
            .filter_state = &g_hw.tex_filter,
        };
        draw(sw, sh, &d);
    }
    const bool shown = swap_or_handle_error();
    if (g_hw.lost) {
        return hw_recover(); /* the frame is gone; the core redraws into the new FBO */
    }
    if (eglGetCurrentContext() == g_context) restore_gl_state(&saved);
    return shown;
}

bool rn_video_present(void) {
    if (g_hw.active) {
        if (g_hw.failed) return false;
        if (g_hw.lost && !hw_recover()) return false;
    }
    if (!g_window || !context_is_live()) return false;
    if (!make_current()) {
        if (g_hw.active && g_hw.lost) return hw_recover();
        return false;
    }
    if (!g_gl_ready) return false;

    if (g_hw.active) return present_hw();

    const rn_video_frame_t* f = rn_get_video_frame();
    if (f->data && f->width && f->height && (!g_tex_valid || g_uploaded_seq != g_frame_seq)) {
        if (upload_frame(f)) {
            g_uploaded_seq = g_frame_seq;
            g_tex_valid = true;
        }
    }
    /* NOTE: f->frame_ready is deliberately left untouched (nativeGetFrameBuffer consumes it). */

    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);

    set_present_state(sw, sh);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g_tex_valid) {
        const draw_src_t d = {
            .tex = g_texture,
            .src_w = g_tex_w, .src_h = g_tex_h,
            .tex_w = g_tex_w, .tex_h = g_tex_h,
            .vbo = g_vbo,
            .filter_state = &g_tex_filter,
        };
        draw(sw, sh, &d);
    }
    return swap_or_handle_error();
}

/* ------------------------------------------------------------------ */
/* Hardware rendering                                                  */
/* ------------------------------------------------------------------ */

static pthread_once_t g_gles_libs_once = PTHREAD_ONCE_INIT;
static void* g_gles3_lib = NULL;
static void* g_gles2_lib = NULL;

static void open_gles_libs(void) {
    g_gles3_lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    g_gles2_lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
}

/* get_proc_address: eglGetProcAddress (extensions and, on current Android, core entry points),
 * falling back to the GLES libraries' exported symbols. May be called from any core thread. */
static retro_proc_address_t hw_get_proc_address(const char* sym) {
    if (!sym) return NULL;
    void* p = (void*)eglGetProcAddress(sym);
    if (!p) {
        pthread_once(&g_gles_libs_once, open_gles_libs);
        if (g_gles3_lib) p = dlsym(g_gles3_lib, sym);
        if (!p && g_gles2_lib) p = dlsym(g_gles2_lib, sym);
    }
    return (retro_proc_address_t)p;
}

static uintptr_t hw_get_current_framebuffer(void) {
    return (uintptr_t)g_hw.fbo;
}

static const char* hw_type_name(enum retro_hw_context_type t) {
    switch (t) {
        case RETRO_HW_CONTEXT_OPENGL:           return "OpenGL";
        case RETRO_HW_CONTEXT_OPENGLES2:        return "OpenGL ES 2";
        case RETRO_HW_CONTEXT_OPENGL_CORE:      return "OpenGL core";
        case RETRO_HW_CONTEXT_OPENGLES3:        return "OpenGL ES 3";
        case RETRO_HW_CONTEXT_OPENGLES_VERSION: return "OpenGL ES (version)";
        case RETRO_HW_CONTEXT_VULKAN:           return "Vulkan";
        default:                                return "other";
    }
}

bool rn_video_hw_set(struct retro_hw_render_callback* cb) {
    if (!cb) return false;
    unsigned need_major, need_minor = 0;
    switch (cb->context_type) {
        case RETRO_HW_CONTEXT_OPENGLES2:
            need_major = 2;
            break;
        case RETRO_HW_CONTEXT_OPENGLES3:
            need_major = 3;
            break;
        case RETRO_HW_CONTEXT_OPENGLES_VERSION:
            need_major = cb->version_major;
            need_minor = cb->version_minor;
            if (need_major < 2 || need_major > 3) {
                VLOGW("Core asked for OpenGL ES %u.%u: unsupported", need_major, need_minor);
                return false;
            }
            break;
        default:
            VLOGW("Core asked for %s (type %d) hardware rendering: only OpenGL ES is supported",
                  hw_type_name(cb->context_type), (int)cb->context_type);
            return false;
    }

    if (!ensure_display()) return false;
    if (g_hw.lost) { destroy_context(false); g_hw.lost = false; }
    if (g_context == EGL_NO_CONTEXT && !ensure_context(true)) return false;
    g_hw_ctx = true; /* from here on, window detach keeps the context current */
    if (!make_hw_current()) {
        VLOGE("Cannot make the GL context current without a window");
        return false;
    }
    if (!g_gl_ready && !init_gl_objects()) return false;

    const bool version_ok = (unsigned)g_es_major > need_major ||
                            ((unsigned)g_es_major == need_major && (unsigned)g_es_minor >= need_minor);
    if (!version_ok) {
        VLOGW("Core needs OpenGL ES %u.%u, context is %d.%d: rejected", need_major, need_minor,
              g_es_major, g_es_minor);
        return false;
    }

    cb->get_current_framebuffer = hw_get_current_framebuffer;
    cb->get_proc_address        = hw_get_proc_address;
    g_hw.cb = *cb;
    g_hw.requested = true;
    VLOGI("HW render accepted: %s (asked %u.%u), depth=%d stencil=%d bottom_left_origin=%d "
          "cache_context=%d, context ES %d.%d", hw_type_name(cb->context_type), need_major,
          need_minor, cb->depth, cb->stencil, cb->bottom_left_origin, cb->cache_context,
          g_es_major, g_es_minor);
    return true;
}

bool rn_video_hw_requested(void) { return g_hw.requested; }

bool rn_video_hw_active(void) { return g_hw.active; }

static GLint gl_get_int(GLenum e) {
    GLint v = 0;
    glGetIntegerv(e, &v);
    return v;
}

/* (Re)allocates the FBO attachments at want_w x want_h, keeping the FBO / texture names. The core's
 * bindings are saved and restored (this can run between frames). Context must be current. */
static bool hw_alloc_fbo(void) {
    unsigned w = g_hw.want_w ? g_hw.want_w : HW_DEFAULT_W;
    unsigned h = g_hw.want_h ? g_hw.want_h : HW_DEFAULT_H;
    const GLint max_tex = gl_get_int(GL_MAX_TEXTURE_SIZE);
    const GLint max_rb  = gl_get_int(GL_MAX_RENDERBUFFER_SIZE);
    GLint max = max_tex;
    if (max_rb > 0 && (max <= 0 || max_rb < max)) max = max_rb;
    if (max > 0) {
        if (w > (unsigned)max) w = (unsigned)max;
        if (h > (unsigned)max) h = (unsigned)max;
    }

    GLint old_active = 0, old_tex = 0, old_rb = 0, old_fb = 0, old_read_fb = 0, old_unpack = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb); /* ES 3: the draw binding */
    if (g_es_major >= 3) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fb);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_unpack);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0); /* NULL pixels must not become a PBO offset */
    }

    if (!g_hw.fbo) glGenFramebuffers(1, &g_hw.fbo);
    if (!g_hw.color_tex) glGenTextures(1, &g_hw.color_tex);

    glBindTexture(GL_TEXTURE_2D, g_hw.color_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_hw.tex_filter = GL_NEAREST;
    glTexImage2D(GL_TEXTURE_2D, 0, g_es_major >= 3 ? GL_RGBA8 : GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, g_hw.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_hw.color_tex, 0);

    if (g_hw.cb.depth || g_hw.cb.stencil) {
        if (!g_hw.depth_rb) glGenRenderbuffers(1, &g_hw.depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g_hw.depth_rb);
        const bool es3 = g_es_major >= 3;
        const bool packed = es3 || has_gl_extension("GL_OES_packed_depth_stencil");
        const bool want_stencil = g_hw.cb.stencil;
        GLenum fmt;
        if (want_stencil && packed)            fmt = GL_DEPTH24_STENCIL8; /* == DEPTH24_STENCIL8_OES */
        else if (es3 || has_gl_extension("GL_OES_depth24")) fmt = GL_DEPTH_COMPONENT24;
        else                                   fmt = GL_DEPTH_COMPONENT16;
        if (want_stencil && !packed) VLOGW("No packed depth/stencil on ES 2: stencil unavailable");
        glRenderbufferStorage(GL_RENDERBUFFER, fmt, (GLsizei)w, (GLsizei)h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_hw.depth_rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  fmt == GL_DEPTH24_STENCIL8 ? g_hw.depth_rb : 0);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (g_es_major >= 3) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)old_read_fb);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old_rb);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
    glActiveTexture((GLenum)old_active);
    if (g_es_major >= 3) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)old_unpack);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        VLOGE("HW framebuffer %ux%u incomplete: 0x%x", w, h, status);
        return false;
    }
    g_hw.fbo_w = w;
    g_hw.fbo_h = h;
    if (g_hw.frame_w > w) g_hw.frame_w = w;
    if (g_hw.frame_h > h) g_hw.frame_h = h;
    VLOGI("HW framebuffer %u: %ux%u (depth=%d stencil=%d)", g_hw.fbo, w, h, g_hw.cb.depth,
          g_hw.cb.stencil);
    return true;
}

/* Clears the fresh FBO to opaque black (only before context_reset: no core state to preserve). */
static void hw_clear_fbo(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_hw.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFFFFFFFu);
    glViewport(0, 0, (GLsizei)g_hw.fbo_w, (GLsizei)g_hw.fbo_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void grow_want(unsigned w, unsigned h) {
    if (w > g_hw.want_w) g_hw.want_w = w;
    if (h > g_hw.want_h) g_hw.want_h = h;
}

bool rn_video_hw_start(unsigned max_w, unsigned max_h) {
    if (!g_hw.requested) return false;
    g_hw.failed = false;
    g_hw.has_frame = false;
    g_hw.frame_w = g_hw.frame_h = 0;
    g_hw.want_w = g_hw.want_h = 0;
    grow_want(max_w, max_h);

    if (g_hw.lost) { destroy_context(false); g_hw.lost = false; }
    if (!ensure_display() || !ensure_context(true)) return false;
    if (g_window && g_surface == EGL_NO_SURFACE && !attach_surface()) destroy_surface();
    if (!make_hw_current()) {
        VLOGE("HW start: cannot make the GL context current");
        return false;
    }
    if (!g_gl_ready && !init_gl_objects()) return false;
    if (!hw_alloc_fbo()) return false;
    hw_clear_fbo();
    reset_bindings();

    g_hw.active = true;
    if (g_hw.cb.context_reset) {
        VLOGI("HW render: context_reset");
        g_hw.cb.context_reset();
    }
    return true;
}

void rn_video_hw_set_size(unsigned w, unsigned h) {
    if (!g_hw.requested) return;
    grow_want(w, h); /* applied by rn_video_hw_begin_frame on the emulation thread */
}

/* Context loss: tell the core, rebuild context + objects + FBO (same window), context_reset. */
static bool hw_recover(void) {
    if (!g_hw.active) return false;
    VLOGW("GL context lost; recreating it for the core");
    g_hw.lost = false;
    if (g_hw.cb.context_destroy) g_hw.cb.context_destroy(); /* GL calls on a lost context are no-ops */
    destroy_context(false);

    bool ok = ensure_display() && ensure_context(true);
    if (ok && g_window) {
        if (!attach_surface()) destroy_surface(); /* continue headless; present will report failure */
    }
    ok = ok && make_hw_current() && (g_gl_ready || init_gl_objects()) && hw_alloc_fbo();
    if (!ok || g_hw.lost) {
        VLOGE("GL context recovery failed");
        g_hw.failed = true;
        g_hw.lost = false;
        return false;
    }
    hw_clear_fbo();
    reset_bindings();
    if (g_hw.cb.context_reset) g_hw.cb.context_reset();
    return true;
}

bool rn_video_hw_begin_frame(void) {
    if (!g_hw.active) return true;
    if (g_hw.failed) return false;
    if (g_hw.lost && !hw_recover()) return false;
    if (!make_hw_current()) {
        if (g_hw.lost) return hw_recover();
        return false;
    }
    if (g_hw.want_w > g_hw.fbo_w || g_hw.want_h > g_hw.fbo_h) {
        const unsigned old_w = g_hw.fbo_w, old_h = g_hw.fbo_h;
        if (!hw_alloc_fbo()) {
            VLOGW("HW framebuffer growth failed; keeping %ux%u", old_w, old_h);
            g_hw.want_w = old_w; /* do not retry every frame */
            g_hw.want_h = old_h;
            if (!old_w || !old_h) { g_hw.failed = true; return false; }
            /* storage may be gone: try to restore the old size once */
            if (!hw_alloc_fbo()) { g_hw.failed = true; return false; }
        }
    }
    return true;
}

void rn_video_hw_on_frame(unsigned* width, unsigned* height) {
    unsigned w = *width, h = *height;
    if (w > g_hw.fbo_w || h > g_hw.fbo_h) {
        grow_want(w, h); /* bigger FBO from the next frame on */
        if (w > g_hw.fbo_w) w = g_hw.fbo_w;
        if (h > g_hw.fbo_h) h = g_hw.fbo_h;
    }
    g_hw.frame_w = w;
    g_hw.frame_h = h;
    g_hw.has_frame = w > 0 && h > 0;
    *width = w;
    *height = h;
}

void rn_video_hw_frame_size(unsigned* width, unsigned* height) {
    *width  = g_hw.has_frame ? g_hw.frame_w : 0;
    *height = g_hw.has_frame ? g_hw.frame_h : 0;
}

/* RGBA bytes (glReadPixels rows, row 0 first) → opaque ARGB_8888 ints, top row first. */
static void rgba_rows_to_argb(const uint8_t* src, uint32_t* dst, unsigned w, unsigned h,
                              bool flip) {
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)(flip ? h - 1 - y : y) * w * 4;
        uint32_t* d = dst + (size_t)y * w;
        for (unsigned x = 0; x < w; x++, s += 4) {
            d[x] = 0xFF000000u | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2];
        }
    }
}

const uint32_t* rn_video_hw_read_frame(unsigned* width, unsigned* height) {
    *width = *height = 0;
    if (!g_hw.active || !g_hw.has_frame || g_hw.failed || !g_hw.fbo) return NULL;
    if (!make_hw_current()) return NULL;
    const unsigned w = g_hw.frame_w, h = g_hw.frame_h;
    const size_t px = (size_t)w * h;
    if (px * 4 > g_read_cap) {
        uint8_t* grown = realloc(g_read_buf, px * 4);
        if (!grown) return NULL;
        g_read_buf = grown;
        g_read_cap = px * 4;
    }
    if (px > g_argb_cap) {
        uint32_t* grown = realloc(g_argb_buf, px * sizeof(uint32_t));
        if (!grown) return NULL;
        g_argb_buf = grown;
        g_argb_cap = px;
    }

    GLint old_fb = 0, old_pack_buf = 0, old_align = 4, old_row = 0, old_skip_rows = 0, old_skip_px = 0;
    const GLenum target = g_es_major >= 3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    glGetIntegerv(g_es_major >= 3 ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &old_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_align);
    if (g_es_major >= 3) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buf);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &old_skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_skip_px);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(target, g_hw.fbo);
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {} /* drop errors the core left */
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, g_read_buf);
    const GLenum err = glGetError();

    glBindFramebuffer(target, (GLuint)old_fb);
    glPixelStorei(GL_PACK_ALIGNMENT, old_align);
    if (g_es_major >= 3) {
        glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
        glPixelStorei(GL_PACK_SKIP_ROWS, old_skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, old_skip_px);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pack_buf);
    }
    if (err != GL_NO_ERROR) {
        VLOGW("HW frame readback failed: 0x%x", err);
        return NULL;
    }

    /* glReadPixels row 0 is the bottom row of the FBO; GL cores (bottom_left_origin) drew the top
     * of the picture there last, so flip to top-down. */
    rgba_rows_to_argb(g_read_buf, g_argb_buf, w, h, g_hw.cb.bottom_left_origin);
    *width = w;
    *height = h;
    return g_argb_buf;
}

void rn_video_hw_context_destroy(void) {
    if (g_hw.active) {
        if (!g_hw.failed && make_hw_current() && g_hw.cb.context_destroy) {
            VLOGI("HW render: context_destroy");
            g_hw.cb.context_destroy();
        }
    }
    /* The context, its FBO and the pbuffer stay until rn_video_shutdown(): the core's
     * retro_unload_game / retro_deinit may still issue GL calls. */
    g_hw.active = false;
    g_hw.requested = false;
    g_hw.has_frame = false;
    g_hw.lost = false;
    g_hw.failed = false;
}

void rn_video_hw_cancel(void) {
    g_hw.requested = false;
    g_hw.active = false;
}

/* ------------------------------------------------------------------ */
/* Bridge hooks                                                        */
/* ------------------------------------------------------------------ */

void rn_video_on_frame(void) {
    g_frame_seq++;
}

void rn_video_shutdown(void) {
    g_hw.lost = false;
    destroy_context(true);
    destroy_surface();
    if (g_display != EGL_NO_DISPLAY) {
        eglTerminate(g_display);
        g_display = EGL_NO_DISPLAY;
        eglReleaseThread();
    }
    release_window();
    free_sources();
    free(g_conv_buf);
    g_conv_buf = NULL;
    g_conv_cap = 0;
    free(g_read_buf);
    g_read_buf = NULL;
    g_read_cap = 0;
    free(g_argb_buf);
    g_argb_buf = NULL;
    g_argb_cap = 0;
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw_ctx = false;
    g_uploaded_seq = g_frame_seq;
    g_rotation = 0;
}
