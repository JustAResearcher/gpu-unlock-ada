/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_APERTURE_COMPAT_H
#define GPU_UNLOCK_APERTURE_COMPAT_H

/* Ubuntu 5.15 still exposes this helper through DRM. Later kernels moved
 * the aperture owner/removal API into linux/aperture.h. A name-only DRM
 * descriptor supplies the old helper's requesting-driver label; it is not
 * registered as a DRM driver and never binds a device. */
#if __has_include(<linux/aperture.h>)
#include <linux/aperture.h>
static inline int gpu_unlock_remove_apertures(struct pci_dev *pdev)
{
	return aperture_remove_conflicting_pci_devices(pdev, "gpu_unlock");
}
#else
#include <drm/drm_aperture.h>
#include <drm/drm_drv.h>
static inline int gpu_unlock_remove_apertures(struct pci_dev *pdev)
{
	static const struct drm_driver requester = { .name = "gpu_unlock" };

	return drm_aperture_remove_conflicting_pci_framebuffers(pdev, &requester);
}
#endif

#endif
