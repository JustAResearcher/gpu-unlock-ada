/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_610_H
#define GPU_UNLOCK_UNLOCK_610_H

struct firmware;
struct pci_dev;

/* Read-only firmware every unlock uses: loaded once, shared by concurrent unlocks. */
struct gpu_unlock_610_shared {
	const struct firmware *gsp;
};

/*
 * Load the shared firmware. pdev is any target card; the request leaves no
 * device resources behind on it. Returns 0 or a negative errno.
 */
int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared);

/* Release the shared firmware once every unlock using it has returned. */
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared);

/*
 * Unlock one cold, unbound supported GPU. The caller holds pci_dev_lock. Returns 0
 * when the unlock and reset checks pass; otherwise a negative errno. Unlocks of different cards may run
 * concurrently with the same shared firmware.
 */
int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared);

#endif
