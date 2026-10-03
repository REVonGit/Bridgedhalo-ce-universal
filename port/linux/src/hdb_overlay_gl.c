/*
 * hdb_overlay_gl.c - draws UZDoom's weapon + HUD over Halo's frame.
 *
 * d3d8_gl.c's D3DDevice_Present calls it after the picture is blitted to
 * the window and before the swap, with the picture's letterboxed rectangle.
 * It loads its own entry points through SDL and puts back every piece of
 * GL state it touches (Present then invalidates d3d8_gl.c's cache anyway).
 */
#include "hdb_hooks.h"

#ifdef HALO_HDBRIDGE

#include <SDL3/SDL.h>
#include <string.h>
#include "gl.h"        /* the port's GL types and constants, its APIENTRY handled */
#include "hdb_bridge.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

typedef void   (GLAPIENTRY *pfn_void_e)(GLenum);
typedef void   (GLAPIENTRY *pfn_GetIntegerv)(GLenum, GLint*);
typedef GLboolean (GLAPIENTRY *pfn_IsEnabled)(GLenum);
typedef void   (GLAPIENTRY *pfn_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void   (GLAPIENTRY *pfn_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void   (GLAPIENTRY *pfn_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (GLAPIENTRY *pfn_BlendEquationSeparate)(GLenum, GLenum);
typedef void   (GLAPIENTRY *pfn_DrawArrays)(GLenum, GLint, GLsizei);
typedef GLuint (GLAPIENTRY *pfn_CreateShaderProgramv)(GLenum, GLsizei, const char* const*);
typedef void   (GLAPIENTRY *pfn_GenProgramPipelines)(GLsizei, GLuint*);
typedef void   (GLAPIENTRY *pfn_UseProgramStages)(GLuint, GLbitfield, GLuint);
typedef void   (GLAPIENTRY *pfn_BindProgramPipeline)(GLuint);
typedef void   (GLAPIENTRY *pfn_UseProgram)(GLuint);
typedef void   (GLAPIENTRY *pfn_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (GLAPIENTRY *pfn_GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef void   (GLAPIENTRY *pfn_CreateVertexArrays)(GLsizei, GLuint*);
typedef void   (GLAPIENTRY *pfn_BindVertexArray)(GLuint);
typedef void   (GLAPIENTRY *pfn_CreateTextures)(GLenum, GLsizei, GLuint*);
typedef void   (GLAPIENTRY *pfn_DeleteTextures)(GLsizei, const GLuint*);
typedef void   (GLAPIENTRY *pfn_TextureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
typedef void   (GLAPIENTRY *pfn_TextureSubImage2D)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
typedef void   (GLAPIENTRY *pfn_TextureParameteri)(GLuint, GLenum, GLint);
typedef void   (GLAPIENTRY *pfn_BindTextureUnit)(GLuint, GLuint);
typedef void   (GLAPIENTRY *pfn_BindSampler)(GLuint, GLuint);
typedef void   (GLAPIENTRY *pfn_BindFramebuffer)(GLenum, GLuint);
typedef void   (GLAPIENTRY *pfn_BindBuffer)(GLenum, GLuint);
typedef void   (GLAPIENTRY *pfn_PixelStorei)(GLenum, GLint);
typedef void   (GLAPIENTRY *pfn_ActiveTexture)(GLenum);
typedef void   (GLAPIENTRY *pfn_BindTexture)(GLenum, GLuint);

static struct {
    int loaded, failed;
    pfn_void_e Enable, Disable;
    pfn_GetIntegerv GetIntegerv;
    pfn_IsEnabled IsEnabled;
    pfn_Viewport Viewport, Scissor;
    pfn_ColorMask ColorMask;
    pfn_BlendFuncSeparate BlendFuncSeparate;
    pfn_BlendEquationSeparate BlendEquationSeparate;
    pfn_DrawArrays DrawArrays;
    pfn_CreateShaderProgramv CreateShaderProgramv;
    pfn_GenProgramPipelines GenProgramPipelines;
    pfn_UseProgramStages UseProgramStages;
    pfn_BindProgramPipeline BindProgramPipeline;
    pfn_UseProgram UseProgram;
    pfn_GetProgramiv GetProgramiv;
    pfn_GetProgramInfoLog GetProgramInfoLog;
    pfn_CreateVertexArrays CreateVertexArrays;
    pfn_BindVertexArray BindVertexArray;
    pfn_CreateTextures CreateTextures;
    pfn_DeleteTextures DeleteTextures;
    pfn_TextureStorage2D TextureStorage2D;
    pfn_TextureSubImage2D TextureSubImage2D;
    pfn_TextureParameteri TextureParameteri;
    pfn_BindTextureUnit BindTextureUnit;
    pfn_BindSampler BindSampler;
    pfn_BindFramebuffer BindFramebuffer;
    pfn_BindBuffer BindBuffer;
    pfn_PixelStorei PixelStorei;
    pfn_ActiveTexture ActiveTexture;
    pfn_BindTexture BindTexture;

    GLuint pipeline, vao, tex;
    GLsizei tex_w, tex_h;
    uint32_t last_frame;
} G = { 0 };

#define LOAD(name) (G.failed |= !(*(void**)&G.name = (void*)SDL_GL_GetProcAddress("gl" #name)))

static const char* k_vs =
    "#version 450 core\n"
    "out gl_PerVertex { vec4 gl_Position; };\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"   /* fullscreen triangle */
    "    uv = vec2(p.x, 1.0 - p.y);\n"                                  /* overlay rows are top-down */
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* k_fs =
    "#version 450 core\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(binding = 0) uniform sampler2D overlay;\n"
    "layout(location = 0) out vec4 color;\n"
    "void main() { color = texture(overlay, uv); }\n";

static GLuint make_stage(GLenum type, const char* src) {
    GLint ok = 0;
    GLuint prog = G.CreateShaderProgramv(type, 1, &src);
    G.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        G.GetProgramInfoLog(prog, sizeof log, NULL, log);
        SDL_Log("[HaloDoomBridge] overlay shader failed: %s", log);
        return 0;
    }
    return prog;
}

static int init_gl(void) {
    GLuint vs, fs;
    if (G.loaded) return !G.failed;
    G.loaded = 1;
    LOAD(Enable); LOAD(Disable); LOAD(GetIntegerv); LOAD(IsEnabled); LOAD(Viewport); LOAD(Scissor);
    LOAD(ColorMask); LOAD(BlendFuncSeparate); LOAD(BlendEquationSeparate); LOAD(DrawArrays);
    LOAD(CreateShaderProgramv); LOAD(GenProgramPipelines); LOAD(UseProgramStages);
    LOAD(BindProgramPipeline); LOAD(UseProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog);
    LOAD(CreateVertexArrays); LOAD(BindVertexArray); LOAD(CreateTextures); LOAD(DeleteTextures);
    LOAD(TextureStorage2D); LOAD(TextureSubImage2D); LOAD(TextureParameteri); LOAD(BindTextureUnit);
    LOAD(BindSampler); LOAD(BindFramebuffer); LOAD(BindBuffer); LOAD(PixelStorei);
    LOAD(ActiveTexture); LOAD(BindTexture);
    if (G.failed) { SDL_Log("[HaloDoomBridge] overlay: missing GL 4.5 entry points"); return 0; }

    vs = make_stage(GL_VERTEX_SHADER, k_vs);
    fs = make_stage(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) { G.failed = 1; return 0; }
    G.GenProgramPipelines(1, &G.pipeline);
    G.UseProgramStages(G.pipeline, GL_VERTEX_SHADER_BIT, vs);
    G.UseProgramStages(G.pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    G.CreateVertexArrays(1, &G.vao);
    G.last_frame = HDB_NONE;
    return 1;
}

/* Copies the newest finished overlay buffer into the texture. */
static int upload(hdb_overlay* o) {
    uint32_t idx = o->front, w, h, frame;
    if (idx >= HDB_OVERLAY_BUFFERS) return 0;
    o->reading = idx;
    if (o->front != idx) { idx = o->front; o->reading = idx; }
    HDB_FENCE_ACQ();

    w = o->width[idx]; h = o->height[idx];
    if (!w || !h || w > HDB_OVERLAY_MAX_W || h > HDB_OVERLAY_MAX_H) { o->reading = HDB_NONE; return 0; }

    if (!G.tex || (GLsizei)w != G.tex_w || (GLsizei)h != G.tex_h) {
        if (G.tex) G.DeleteTextures(1, &G.tex);
        G.CreateTextures(GL_TEXTURE_2D, 1, &G.tex);
        G.TextureStorage2D(G.tex, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        /* Doom art is pixel art and UZDoom renders at the picture's size. */
        G.TextureParameteri(G.tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        G.TextureParameteri(G.tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        G.TextureParameteri(G.tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        G.TextureParameteri(G.tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        G.tex_w = (GLsizei)w; G.tex_h = (GLsizei)h;
        G.last_frame = HDB_NONE;
    }

    frame = o->frame_id;
    if (frame != G.last_frame) {
        G.TextureSubImage2D(G.tex, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, o->pixels[idx]);
        G.last_frame = frame;
    }
    o->reading = HDB_NONE;
    return 1;
}

void hdb_overlay_draw(int x, int y, int w, int h) {
    hdb_shared* shm = hdb_bridge_shared();
    GLint prog, pipe, vao, fbo, unpack_buf, unpack_row, unpack_align, active_tex, tex0, sampler0;
    GLint viewport[4], scissor_box[4], blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint blend_eq_rgb, blend_eq_a;
    GLboolean blend, depth, cull, scissor, stencil, srgb, cmask[4];

    hdb_bridge_set_picture_size(w, h);
    if (!shm || !hdb_bridge_driving() || w <= 0 || h <= 0) return;
    if (!init_gl()) return;

    /* --- save ---------------------------------------------------- */
    G.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
    G.GetIntegerv(GL_PROGRAM_PIPELINE_BINDING, &pipe);
    G.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    G.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    G.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buf);
    G.GetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    G.GetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align);
    G.GetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    G.ActiveTexture(GL_TEXTURE0);
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    G.GetIntegerv(GL_SAMPLER_BINDING, &sampler0);
    G.GetIntegerv(GL_VIEWPORT, viewport);
    G.GetIntegerv(GL_SCISSOR_BOX, scissor_box);
    G.GetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    G.GetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    G.GetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    G.GetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    G.GetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);
    G.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a);
    {
        GLint m[4];
        G.GetIntegerv(GL_COLOR_WRITEMASK, m);
        cmask[0] = (GLboolean)m[0]; cmask[1] = (GLboolean)m[1]; cmask[2] = (GLboolean)m[2]; cmask[3] = (GLboolean)m[3];
    }
    blend = G.IsEnabled(GL_BLEND);
    depth = G.IsEnabled(GL_DEPTH_TEST);
    cull = G.IsEnabled(GL_CULL_FACE);
    scissor = G.IsEnabled(GL_SCISSOR_TEST);
    stencil = G.IsEnabled(GL_STENCIL_TEST);
    srgb = G.IsEnabled(GL_FRAMEBUFFER_SRGB);

    /* --- draw ---------------------------------------------------- */
    G.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    G.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (upload(&shm->overlay)) {
        G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        G.UseProgram(0);
        G.BindProgramPipeline(G.pipeline);
        G.BindVertexArray(G.vao);
        G.BindTextureUnit(0, G.tex);
        G.BindSampler(0, 0);
        G.Viewport(x, y, w, h);
        G.Disable(GL_DEPTH_TEST);
        G.Disable(GL_CULL_FACE);
        G.Disable(GL_SCISSOR_TEST);
        G.Disable(GL_STENCIL_TEST);
        G.Disable(GL_FRAMEBUFFER_SRGB);
        G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        G.Enable(GL_BLEND);
        G.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        G.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        G.DrawArrays(GL_TRIANGLES, 0, 3);
    }

    /* --- restore ------------------------------------------------- */
    G.BindTextureUnit(0, (GLuint)tex0);
    G.BindSampler(0, (GLuint)sampler0);
    G.ActiveTexture((GLenum)active_tex);
    G.BindVertexArray((GLuint)vao);
    G.BindProgramPipeline((GLuint)pipe);
    G.UseProgram((GLuint)prog);
    G.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)fbo);
    G.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack_buf);
    G.PixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    G.PixelStorei(GL_UNPACK_ALIGNMENT, unpack_align);
    G.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    G.Scissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    G.BlendEquationSeparate((GLenum)blend_eq_rgb, (GLenum)blend_eq_a);
    G.BlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a, (GLenum)blend_dst_a);
    G.ColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    if (blend) G.Enable(GL_BLEND); else G.Disable(GL_BLEND);
    if (depth) G.Enable(GL_DEPTH_TEST); else G.Disable(GL_DEPTH_TEST);
    if (cull) G.Enable(GL_CULL_FACE); else G.Disable(GL_CULL_FACE);
    if (scissor) G.Enable(GL_SCISSOR_TEST); else G.Disable(GL_SCISSOR_TEST);
    if (stencil) G.Enable(GL_STENCIL_TEST); else G.Disable(GL_STENCIL_TEST);
    if (srgb) G.Enable(GL_FRAMEBUFFER_SRGB); else G.Disable(GL_FRAMEBUFFER_SRGB);
}

#endif /* HALO_HDBRIDGE */
