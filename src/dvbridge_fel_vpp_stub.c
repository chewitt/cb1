/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FEL VPP backend for platforms without VA-API (e.g. V4L2 stateless decoders
 * on ARM SoCs). dvbridge_fel_vpp_create() never succeeds, so callers take the
 * existing GPU shader FEL upscaling path. The remaining entry points are only
 * reachable with a valid instance and report failure defensively. */
#include "dvbridge_fel_vpp.h"
#include <stddef.h>

struct dvbridge_fel_vpp *dvbridge_fel_vpp_create(AVBufferRef *device, int width, int height)
{
    (void)device; (void)width; (void)height;
    return NULL;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_acquire(struct dvbridge_fel_vpp *vpp,
    const AVFrame *source, const struct dvbridge_fel_vpp_image **image)
{
    (void)vpp; (void)source;
    if (image)
        *image = NULL;
    return DVBRIDGE_VPP_FAILED;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_acquire_chroma(struct dvbridge_fel_vpp *vpp,
    const AVFrame *source, enum pl_chroma_location chroma,
    const struct dvbridge_fel_vpp_image **image)
{
    (void)chroma;
    return dvbridge_fel_vpp_acquire(vpp, source, image);
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_prefetch_chroma(struct dvbridge_fel_vpp *vpp,
    const AVFrame *source, enum pl_chroma_location chroma,
    const struct dvbridge_fel_vpp_image **image)
{
    (void)chroma;
    return dvbridge_fel_vpp_acquire(vpp, source, image);
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_finish(struct dvbridge_fel_vpp *vpp,
    const struct dvbridge_fel_vpp_image *image)
{
    (void)vpp; (void)image;
    return DVBRIDGE_VPP_FAILED;
}

void dvbridge_fel_vpp_release(struct dvbridge_fel_vpp *vpp, const struct dvbridge_fel_vpp_image *image)
{
    (void)vpp; (void)image;
}

bool dvbridge_fel_vpp_destroy(struct dvbridge_fel_vpp **vpp)
{
    if (vpp)
        *vpp = NULL;
    return true;
}
