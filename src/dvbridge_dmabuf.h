/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DVBRIDGE_DMABUF_H
#define DVBRIDGE_DMABUF_H
#include "dvbridge_gl_frame.h"
#include <EGL/egl.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>

/* How the mapped DRM PRIME descriptor is laid out. */
enum dvbridge_dmabuf_source {
    DVBRIDGE_DMABUF_P010_LAYERS, /* VA-API export: R16 + GR1616 layers */
    DVBRIDGE_DMABUF_P010,        /* one P010 layer, two planes */
    DVBRIDGE_DMABUF_NV12,        /* one NV12 layer, two planes */
    DVBRIDGE_DMABUF_NV15,        /* one NV15 layer: unpacked to P010 textures */
};

struct dvbridge_dmabuf {
    EGLDisplay display;
    AVFrame *mapping;
    EGLImage images[2];
    struct dvbridge_gl_plane planes[3];
    enum dvbridge_gl_layout layout;
    int width, height;
    enum dvbridge_dmabuf_source source;
};
struct dvbridge_dmabuf_key {
    uint64_t display, context, pool, width, height, objects_count;
    uint64_t objects[AV_DRM_MAX_PLANES][4];
    uint64_t layers[2][5];
};
bool dvbridge_dmabuf_make_key(struct dvbridge_dmabuf_key *out,
    const AVDRMFrameDescriptor *desc, int width, int height, const void *pool,
    EGLDisplay display, EGLContext context);
bool dvbridge_dmabuf_key_equal(const struct dvbridge_dmabuf_key *a,
                              const struct dvbridge_dmabuf_key *b);
bool dvbridge_dmabuf_validate_p010(const AVDRMFrameDescriptor *desc, int width, int height);
/* VAAPI frames are mapped to DRM PRIME; DRM PRIME frames (V4L2 stateless
 * decoders) are referenced directly. NV15 has no GL format and is unpacked by
 * the GPU into P010-identical UNORM16 textures (GL_EXT_texture_norm16), which
 * are a copy: such imports must not be cached by dmabuf identity, and
 * dvbridge_dmabuf_make_key() only accepts the VA-API layout.
 * Current EGL/GLES context required. Does not transfer pixels through the CPU. */
bool dvbridge_dmabuf_import(struct dvbridge_dmabuf *out, const AVFrame *frame);
/* Mapping synchronizes each new decoder picture even when its EGL import is reused. */
bool dvbridge_dmabuf_map(struct dvbridge_dmabuf *out, const AVFrame *frame);
bool dvbridge_dmabuf_import_mapped(struct dvbridge_dmabuf *frame);
void dvbridge_dmabuf_release(struct dvbridge_dmabuf *frame);
#endif
