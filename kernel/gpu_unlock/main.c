// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "unlock_610.h"
#include "arch_610.h"

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported RTX 4090/GA10x cards; AD103 and AD106 require an exact single address");

static bool ad103_experimental;
module_param(ad103_experimental, bool, 0444);
MODULE_PARM_DESC(ad103_experimental, "permit only the exact 10de:2705 AD103 canary with a single explicit PCI address");
static bool ad106_experimental;
module_param(ad106_experimental, bool, 0444);
MODULE_PARM_DESC(ad106_experimental, "permit only the exact 10de:2805 AD106 target with a single explicit PCI address; untested on silicon");
static bool ad107_experimental;
module_param(ad107_experimental, bool, 0444);
MODULE_PARM_DESC(ad107_experimental, "permit only the exact 10de:28a0 AD107 (RTX 4060 Laptop) target with a single explicit PCI address; untested on silicon");

static bool rtx4090d_experimental;
module_param(rtx4090d_experimental, bool, 0444);
MODULE_PARM_DESC(rtx4090d_experimental, "permit experimental RTX 4090 D (10de:2685) targets");

/* RTX 4090 and supported desktop GA102/GA103/GA104/GA106 GPU functions.
 * Professional parts already have full FP32-accumulate tensor issue rate.
 */
static const unsigned short supported_ids[] = {
	/* AD102: GeForce RTX 4090 */
	0x2684,
	/* GA102 */
	0x2200, 0x2203, 0x2204, 0x2205, 0x2206, 0x2207, 0x2208, 0x220a,
	0x220d, 0x2216, 0x222b, 0x222f,
	/* GA104 */
	0x2480, 0x2482, 0x2483, 0x2484, 0x2486, 0x2487, 0x2488, 0x2489,
	0x248a, 0x248c, 0x248d, 0x248e, 0x249c, 0x249d, 0x249f, 0x24a0,
	0x24a4, 0x24ac, 0x24ad, 0x24af, 0x24bf, 0x24c0, 0x24c7, 0x24c8,
	0x24c9, 0x24dc, 0x24dd, 0x24df, 0x24e0,
	/* GA103 (untested) */
	0x2414,
	/* GA106 (validated only on RTX 3060 12GB 0x2504) */
	0x2501, 0x2503, 0x2504, 0x2505, 0x2507, 0x2508, 0x2509, 0x252f,
	0x2544,
};

static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static struct gpu_unlock_610_shared shared;

/* True for a card this load may act on at all. */
static bool supported(const struct pci_dev *pdev)
{
	unsigned int i;

	if (pdev->device == GU_AD103_RTX_4070_TI_SUPER)
		return ad103_experimental;
	if (pdev->device == GU_AD102_RTX_4090_D)
		return rtx4090d_experimental;
	if (pdev->device == GU_AD106_RTX_4060_TI_16GB)
		return ad106_experimental;
	if (pdev->device == GU_AD107_RTX_4060_LAPTOP)
		return ad107_experimental;
	for (i = 0; i < ARRAY_SIZE(supported_ids); i++)
		if (pdev->device == supported_ids[i])
			return true;
	return false;
}

/* The next supported NVIDIA card after from, like pci_get_device(). */
static struct pci_dev *next_supported(struct pci_dev *from)
{
	struct pci_dev *pdev = from;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		if (supported(pdev))
			return pdev;
	return NULL;
}

/* Runs concurrently for each card; consumes the reference it is given. */
static void unlock_one(void *data, async_cookie_t cookie)
{
	struct pci_dev *pdev = data;
	int ret;

	pci_dev_lock(pdev);
	ret = pdev->driver ? -EBUSY : gpu_unlock_610_unlock(pdev, &shared);
	pci_dev_unlock(pdev);
	if (ret)
		pci_err(pdev, "gpu_unlock: unlock failed (%d)\n", ret);
	else
		pci_info(pdev, "gpu_unlock: unlocked\n");
	pci_dev_put(pdev);
}

/* True when the comma-separated targets list names this PCI device. */
static bool listed(const char *name)
{
	const char *entry, *end;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		if (strlen(name) == end - entry && !strncasecmp(name, entry, end - entry))
			return true;
	}
	return false;
}

static void report_unmatched(void)
{
	const char *entry, *end;
	struct pci_dev *pdev;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		pdev = NULL;
		while ((pdev = next_supported(pdev)))
			if (strlen(pci_name(pdev)) == end - entry &&
			    !strncasecmp(pci_name(pdev), entry, end - entry))
				break;
		if (pdev)
			pci_dev_put(pdev);
		else
			pr_err("gpu_unlock: no supported card at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
}

static int __init gpu_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	bool all;
	int ret;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets)
		return 0;
	all = !strcmp(targets, "all");
	if (!all)
		report_unmatched();
	while ((pdev = next_supported(pdev))) {
		if (pdev->device == GU_AD103_RTX_4070_TI_SUPER &&
		    (all || strcasecmp(targets, pci_name(pdev)))) {
			pci_err(pdev, "gpu_unlock: experimental AD103 requires a single exact PCI target\n");
			continue;
		}
		if (pdev->device == GU_AD106_RTX_4060_TI_16GB &&
		    (all || strcasecmp(targets, pci_name(pdev)))) {
			pci_err(pdev, "gpu_unlock: experimental AD106 requires a single exact PCI target\n");
			continue;
		}
		if (pdev->device == GU_AD107_RTX_4060_LAPTOP &&
		    (all || strcasecmp(targets, pci_name(pdev)))) {
			pci_err(pdev, "gpu_unlock: experimental AD107 requires a single exact PCI target\n");
			continue;
		}
		if (!all && !listed(pci_name(pdev)))
			continue;
		if (!shared.gsp) {
			ret = gpu_unlock_610_load_shared(pdev, &shared);
			if (ret) {
				pci_err(pdev, "gpu_unlock: firmware load failed (%d)\n", ret);
				pci_dev_put(pdev);
				return 0;
			}
		}
		async_schedule_domain(unlock_one, pci_dev_get(pdev), &unlock_domain);
	}
	/* nvidia loads once this returns: every card must be finished by then. */
	async_synchronize_full_domain(&unlock_domain);
	gpu_unlock_610_release_shared(&shared);
	return 0;
}

static void __exit gpu_unlock_exit(void)
{
}

module_init(gpu_unlock_init);
module_exit(gpu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RTX 4090, experimental RTX 4070 Ti SUPER and supported GA10x pre-driver tensor unlock");
MODULE_VERSION("1.1.1");
