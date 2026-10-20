/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Import validated VAAPI or DRM PRIME DMA-BUF planes without copying decoded video to the CPU. */
#include "dvbridge_dmabuf.h"
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

bool dvbridge_dmabuf_make_key(struct dvbridge_dmabuf_key *out,
    const AVDRMFrameDescriptor *desc, int width, int height, const void *pool,
    EGLDisplay display, EGLContext context)
{
    if (!out || !pool || display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT ||
        !dvbridge_dmabuf_validate_p010(desc,width,height))
        return false;
    memset(out,0,sizeof(*out));
    out->display=(uintptr_t)display; out->context=(uintptr_t)context;
    out->pool=(uintptr_t)pool; out->width=width; out->height=height;
    out->objects_count=desc->nb_objects;
    for (int i=0;i<desc->nb_objects;++i) {
        struct stat status;
        if (fstat(desc->objects[i].fd,&status) != 0)
            return false;
        out->objects[i][0]=status.st_dev; out->objects[i][1]=status.st_ino;
        out->objects[i][2]=desc->objects[i].size;
        out->objects[i][3]=desc->objects[i].format_modifier;
    }
    for (int i=0;i<2;++i) {
        const AVDRMLayerDescriptor *layer=&desc->layers[i];
        out->layers[i][0]=layer->format; out->layers[i][1]=layer->nb_planes;
        out->layers[i][2]=layer->planes[0].object_index;
        out->layers[i][3]=layer->planes[0].offset; out->layers[i][4]=layer->planes[0].pitch;
    }
    return true;
}

bool dvbridge_dmabuf_key_equal(const struct dvbridge_dmabuf_key *a,
                              const struct dvbridge_dmabuf_key *b)
{
    return a && b && memcmp(a,b,sizeof(*a)) == 0;
}

bool dvbridge_dmabuf_validate_p010(const AVDRMFrameDescriptor *desc, int width, int height)
{
    if (!desc || width < 1 || width > 4096 || height < 1 || height > 2304 ||
        desc->nb_layers != 2 || desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES)
        return false;
    for (int i = 0; i < 2; ++i) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];
        const AVDRMPlaneDescriptor *plane = &layer->planes[0];
        /* Hades RC1 (9a06cf50), patch 324ceb05: NVIDIA exports RG1616 chroma.
         * Keep the declared FourCC for EGL, exactly as for the base-layer import. */
        const bool format = i ? layer->format == DRM_FORMAT_GR1616 || layer->format == DRM_FORMAT_RG1616
                              : layer->format == DRM_FORMAT_R16;
        if (layer->nb_planes != 1 || !format ||
            plane->object_index < 0 || plane->object_index >= desc->nb_objects)
            return false;
        const AVDRMObjectDescriptor *object = &desc->objects[plane->object_index];
        uint64_t row_bytes = i ? ((width + 1) / 2) * 4 : width * 2;
        uint64_t rows = i ? (height + 1) / 2 : height;
        if (object->fd < 0 || plane->offset < 0 || plane->offset > INT_MAX ||
            plane->pitch <= 0 || plane->pitch > INT_MAX || (uint64_t)plane->pitch < row_bytes ||
            (uint64_t)plane->offset >= object->size)
            return false;
        if (object->format_modifier == DRM_FORMAT_MOD_LINEAR &&
            (uint64_t)plane->offset + (rows - 1) * plane->pitch + row_bytes > object->size)
            return false;
    }
    return true;
}

void dvbridge_dmabuf_release(struct dvbridge_dmabuf *frame)
{
    if (!frame)
        return;
    PFNEGLDESTROYIMAGEKHRPROC destroy = (void *)eglGetProcAddress("eglDestroyImageKHR");
    for (int i = 0; i < 2; ++i) {
        if (frame->planes[i].texture)
            glDeleteTextures(1, &frame->planes[i].texture);
        if (frame->images[i] && destroy)
            destroy(frame->display, frame->images[i]);
    }
    av_frame_free(&frame->mapping);
    memset(frame, 0, sizeof(*frame));
}

/* The (layer, plane) of plane @index for a two-layer or a one-layer, two-plane descriptor. */
static bool drm_plane(const AVDRMFrameDescriptor *desc, int index,
                      const AVDRMLayerDescriptor **layer, const AVDRMPlaneDescriptor **plane)
{
    if (desc->nb_layers == 2 && desc->layers[index].nb_planes == 1) {
        *layer = &desc->layers[index];
        *plane = &desc->layers[index].planes[0];
    } else if (desc->nb_layers == 1 && desc->layers[0].nb_planes == 2) {
        *layer = &desc->layers[0];
        *plane = &desc->layers[0].planes[index];
    } else {
        return false;
    }
    return (*plane)->object_index >= 0 && (*plane)->object_index < desc->nb_objects &&
           desc->objects[(*plane)->object_index].fd >= 0;
}

static bool drm_source(const AVDRMFrameDescriptor *desc, int width, int height,
                       enum dvbridge_dmabuf_source *source)
{
    if (!desc || width < 2 || width > 4096 || height < 2 || height > 2304 ||
        desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES)
        return false;
    if (desc->nb_layers == 2) {
        *source = DVBRIDGE_DMABUF_P010_LAYERS;
        return dvbridge_dmabuf_validate_p010(desc, width, height);
    }
    if (desc->nb_layers != 1 || desc->layers[0].nb_planes != 2)
        return false;
    for (int i = 0; i < desc->nb_objects; ++i)
        if (desc->objects[i].format_modifier != DRM_FORMAT_MOD_LINEAR &&
            desc->objects[i].format_modifier != DRM_FORMAT_MOD_INVALID)
            return false;
    switch (desc->layers[0].format) {
    case DRM_FORMAT_P010: *source = DVBRIDGE_DMABUF_P010; break;
    case DRM_FORMAT_NV12: *source = DVBRIDGE_DMABUF_NV12; break;
    case DRM_FORMAT_NV15: *source = DVBRIDGE_DMABUF_NV15; break;
    default: return false;
    }
    /* Both planes must hold their rows within their objects. */
    for (int i = 0; i < 2; ++i) {
        const AVDRMLayerDescriptor *layer;
        const AVDRMPlaneDescriptor *plane;
        if (!drm_plane(desc, i, &layer, &plane))
            return false;
        const AVDRMObjectDescriptor *object = &desc->objects[plane->object_index];
        uint64_t samples = i ? ((width + 1) / 2) * 2 : width;
        uint64_t row_bytes = *source == DVBRIDGE_DMABUF_NV15 ? (samples * 10 + 7) / 8 :
                             *source == DVBRIDGE_DMABUF_P010 ? samples * 2 : samples;
        uint64_t rows = i ? (height + 1) / 2 : height;
        if (plane->offset < 0 || plane->pitch <= 0 || (uint64_t)plane->pitch < row_bytes ||
            (uint64_t)plane->offset + (rows - 1) * plane->pitch + row_bytes > object->size)
            return false;
    }
    return true;
}

bool dvbridge_dmabuf_map(struct dvbridge_dmabuf *out, const AVFrame *source)
{
    if (out && !out->mapping && source && source->format == AV_PIX_FMT_DRM_PRIME && source->data[0]) {
        EGLDisplay display = eglGetCurrentDisplay();
        enum dvbridge_dmabuf_source kind;
        if (display == EGL_NO_DISPLAY ||
            !drm_source((const AVDRMFrameDescriptor *)source->data[0], source->width,
                        source->height, &kind))
            return false;
        struct dvbridge_dmabuf candidate = {.display = display,
            .layout = kind == DVBRIDGE_DMABUF_NV12 ? DVBRIDGE_NV12 : DVBRIDGE_P010,
            .width = source->width, .height = source->height, .source = kind};
        /* The reference keeps the decoder's buffer alive while it is imported. */
        candidate.mapping = av_frame_clone(source);
        if (!candidate.mapping)
            return false;
        *out = candidate;
        return true;
    }
    if (!out || out->mapping || !source || source->format != AV_PIX_FMT_VAAPI ||
        !source->hw_frames_ctx || source->width < 1 || source->height < 1 ||
        source->width > 3840 || source->height > 2160)
        return false;
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY)
        return false;
    AVHWFramesContext *hw = (void *)source->hw_frames_ctx->data;
    if (hw->sw_format != AV_PIX_FMT_P010 || hw->width < source->width || hw->height < source->height)
        return false;
    struct dvbridge_dmabuf candidate = {.display = display, .layout = DVBRIDGE_P010,
                                        .width=hw->width, .height=hw->height,
                                        .source = DVBRIDGE_DMABUF_P010_LAYERS};
    candidate.mapping = av_frame_alloc();
    if (!candidate.mapping)
        return false;
    candidate.mapping->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(candidate.mapping, source, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0)
        goto fail;
    const AVDRMFrameDescriptor *desc = (void *)candidate.mapping->data[0];
    if (!dvbridge_dmabuf_validate_p010(desc, hw->width, hw->height))
        goto fail;
    *out=candidate;
    return true;
fail:
    dvbridge_dmabuf_release(&candidate);
    return false;
}

static bool import_plane(EGLDisplay display, PFNEGLCREATEIMAGEKHRPROC create,
                         PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind, bool modifiers,
                         const AVDRMFrameDescriptor *desc, int index, uint32_t fourcc,
                         int width, int height, int fold, EGLImage *image, GLuint *texture)
{
    const AVDRMLayerDescriptor *layer;
    const AVDRMPlaneDescriptor *plane;
    if (!drm_plane(desc, index, &layer, &plane))
        return false;
    const AVDRMObjectDescriptor *object = &desc->objects[plane->object_index];
    if (object->format_modifier != DRM_FORMAT_MOD_INVALID &&
        object->format_modifier != DRM_FORMAT_MOD_LINEAR && !modifiers)
        return false;
    EGLint attrs[24] = {
        EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, object->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)plane->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)(plane->pitch * fold), EGL_NONE};
    if (modifiers && object->format_modifier != DRM_FORMAT_MOD_INVALID) {
        attrs[12] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[13] = (EGLint)(object->format_modifier & UINT32_MAX);
        attrs[14] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[15] = (EGLint)(object->format_modifier >> 32);
        attrs[16] = EGL_NONE;
    }
    *image = create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (!*image)
        return false;
    GLint previous;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    bind(GL_TEXTURE_2D, *image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, previous);
    return glGetError() == GL_NO_ERROR;
}

/* NV15: 10-bit samples packed LSB-first, 4 samples in 5 bytes. Each output
 * texel is the sample MSB-aligned in UNORM16 (value << 6), i.e. the texel a
 * P010 import would give, so the rest of CB1 sees an ordinary P010 picture.
 * The raw plane may be imported with u_fold rows per texture row, row y
 * starting u_pitch bytes after row y - 1. */
static const char nv15_vertex[] =
    "#version 300 es\n"
    "void main()\n"
    "{\n"
    "  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char nv15_fragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_source;\n"
    "uniform int u_chroma;\n"
    "uniform int u_fold;\n"
    "uniform int u_pitch;\n"
    "out highp vec4 o_colour;\n"
    "uint byteAt(int x, int y)\n"
    "{\n"
    "  ivec2 at = ivec2(x + (y % u_fold) * u_pitch, y / u_fold);\n"
    "  return uint(texelFetch(u_source, at, 0).r * 255.0 + 0.5);\n"
    "}\n"
    "float sampleAt(int index, int y)\n"
    "{\n"
    "  int bit = index * 10;\n"
    "  int b = bit >> 3;\n"
    "  uint word = byteAt(b, y) | (byteAt(b + 1, y) << 8u);\n"
    "  uint value = (word >> uint(bit & 7)) & 1023u;\n"
    "  return float(value << 6u) / 65535.0;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  if (u_chroma == 0)\n"
    "    o_colour = vec4(sampleAt(p.x, p.y), 0.0, 0.0, 1.0);\n"
    "  else\n"
    "    o_colour = vec4(sampleAt(2 * p.x, p.y), sampleAt(2 * p.x + 1, p.y), 0.0, 1.0);\n"
    "}\n";

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#endif
#ifndef GL_RG16_EXT
#define GL_RG16_EXT 0x822C
#endif

/* One program and framebuffer per GL context (CB1 uses a single long-lived one). */
static struct {
    EGLContext context;
    GLuint program, framebuffer;
    GLint source, chroma, fold, pitch;
} nv15_unpack;

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool nv15_unpack_init(void)
{
    EGLContext context = eglGetCurrentContext();
    if (nv15_unpack.context == context && nv15_unpack.program)
        return true;
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions || !strstr(extensions, "GL_EXT_texture_norm16"))
        return false;
    GLuint vertex = compile(GL_VERTEX_SHADER, nv15_vertex);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, nv15_fragment);
    GLuint program = vertex && fragment ? glCreateProgram() : 0;
    GLint linked = GL_FALSE;
    if (program) {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (linked != GL_TRUE) {
        if (program)
            glDeleteProgram(program);
        return false;
    }
    nv15_unpack.context = context;
    nv15_unpack.program = program;
    nv15_unpack.source = glGetUniformLocation(program, "u_source");
    nv15_unpack.chroma = glGetUniformLocation(program, "u_chroma");
    nv15_unpack.fold = glGetUniformLocation(program, "u_fold");
    nv15_unpack.pitch = glGetUniformLocation(program, "u_pitch");
    glGenFramebuffers(1, &nv15_unpack.framebuffer);
    return nv15_unpack.framebuffer != 0;
}

/* Unpack two raw R8 NV15 plane textures into new UNORM16 textures in @frame. */
static bool nv15_unpack_planes(struct dvbridge_dmabuf *frame, const GLuint raw[2],
                               const int fold[2], const int pitch[2])
{
    if (!nv15_unpack_init())
        return false;
    const GLenum formats[2] = {GL_R16_EXT, GL_RG16_EXT};
    GLint framebuffer, program, viewport[4], texture, active;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    const GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(nv15_unpack.program);
    glUniform1i(nv15_unpack.source, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, nv15_unpack.framebuffer);
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        const int width = i ? (frame->width + 1) / 2 : frame->width;
        const int height = i ? (frame->height + 1) / 2 : frame->height;
        glGenTextures(1, &frame->planes[i].texture);
        glBindTexture(GL_TEXTURE_2D, frame->planes[i].texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, formats[i], width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               frame->planes[i].texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ok = false;
            break;
        }
        glViewport(0, 0, width, height);
        glBindTexture(GL_TEXTURE_2D, raw[i]);
        glUniform1i(nv15_unpack.chroma, i);
        glUniform1i(nv15_unpack.fold, fold[i]);
        glUniform1i(nv15_unpack.pitch, pitch[i]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        frame->planes[i].width = width;
        frame->planes[i].height = height;
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glUseProgram(program);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active);
    if (blend)
        glEnable(GL_BLEND);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    return ok && glGetError() == GL_NO_ERROR;
}

bool dvbridge_dmabuf_import_mapped(struct dvbridge_dmabuf *out)
{
    if (!out || !out->mapping || out->images[0] || out->images[1] ||
        out->display != eglGetCurrentDisplay())
        return false;
    PFNEGLCREATEIMAGEKHRPROC create = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!create || !destroy || !bind)
        return false;
    struct dvbridge_dmabuf candidate=*out;
    EGLDisplay display=candidate.display;
    const AVDRMFrameDescriptor *desc=(void*)candidate.mapping->data[0];
    enum dvbridge_dmabuf_source kind;
    if (!drm_source(desc,candidate.width,candidate.height,&kind) || kind != candidate.source)
        return false;
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool modifiers = extensions && strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    const int chroma_width = (candidate.width + 1) / 2, chroma_height = (candidate.height + 1) / 2;

    if (kind == DVBRIDGE_DMABUF_NV15) {
        /* Rows of width samples (luma) or width/2 Cb/Cr pairs, 10 bits each. */
        const int row_bytes = (candidate.width * 10 + 7) / 8;
        const int heights[2] = {candidate.height, chroma_height};
        EGLImage raw_images[2] = {0};
        GLuint raw[2] = {0};
        int fold[2], pitch[2];
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i) {
            const AVDRMLayerDescriptor *layer;
            const AVDRMPlaneDescriptor *plane;
            if (!drm_plane(desc, i, &layer, &plane)) {
                ok = false;
                break;
            }
            const AVDRMObjectDescriptor *object = &desc->objects[plane->object_index];
            pitch[i] = (int)plane->pitch;
            fold[i] = 1;
            while ((pitch[i] * fold[i]) % 64)
                fold[i] *= 2;
            if ((uint64_t)plane->offset + (uint64_t)pitch[i] * fold[i] *
                    ((heights[i] + fold[i] - 1) / fold[i]) > object->size)
                fold[i] = 1;
            ok = import_plane(display, create, bind, modifiers, desc, i, DRM_FORMAT_R8,
                              fold[i] == 1 ? row_bytes : pitch[i] * fold[i],
                              (heights[i] + fold[i] - 1) / fold[i], fold[i],
                              &raw_images[i], &raw[i]);
        }
        ok = ok && nv15_unpack_planes(&candidate, raw, fold, pitch);
        /* GL keeps the raw imports alive until the unpack draws complete. */
        for (int i = 0; i < 2; ++i) {
            if (raw[i])
                glDeleteTextures(1, &raw[i]);
            if (raw_images[i])
                destroy(display, raw_images[i]);
        }
        if (!ok)
            goto fail;
        *out = candidate;
        return true;
    }

    for (int i = 0; i < 2; ++i) {
        const AVDRMLayerDescriptor *layer;
        const AVDRMPlaneDescriptor *plane;
        if (!drm_plane(desc, i, &layer, &plane))
            goto fail;
        uint32_t fourcc = layer->format;
        if (kind == DVBRIDGE_DMABUF_P010)
            fourcc = i ? DRM_FORMAT_GR1616 : DRM_FORMAT_R16;
        else if (kind == DVBRIDGE_DMABUF_NV12)
            fourcc = i ? DRM_FORMAT_GR88 : DRM_FORMAT_R8;
        int width = i ? chroma_width : candidate.width;
        int height = i ? chroma_height : candidate.height;
        if (!import_plane(display, create, bind, modifiers, desc, i, fourcc, width, height, 1,
                          &candidate.images[i], &candidate.planes[i].texture))
            goto fail;
        candidate.planes[i].width = width;
        candidate.planes[i].height = height;
    }
    *out = candidate;
    return true;
fail:
    candidate.mapping=NULL;
    dvbridge_dmabuf_release(&candidate);
    return false;
}

bool dvbridge_dmabuf_import(struct dvbridge_dmabuf *out, const AVFrame *source)
{
    if (!dvbridge_dmabuf_map(out,source))
        return false;
    if (dvbridge_dmabuf_import_mapped(out))
        return true;
    dvbridge_dmabuf_release(out);
    return false;
}
