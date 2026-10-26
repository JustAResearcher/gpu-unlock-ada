// SPDX-License-Identifier: GPL-2.0-only
/*
 * gpu_peek: read-only sampler for the registers gpu_unlock reads and writes.
 *
 * Every access is a single 32-bit load from BAR0. This module never writes
 * config space, never writes BAR space, never runs firmware, never resets the
 * card and does not enable the device. It exists to answer "what do these
 * registers hold on this card, before the driver touches it".
 *
 * The offsets are the ones in kernel/gpu_unlock/gfw_eval.h and
 * kernel/gpu_unlock/unlock_610.c (shared GA102/AD102 offsets). They are ordinary dev
 * register windows, so reading them is safe at any time; what the values mean
 * depends on when you read them.
 *
 * Usage:
 *   insmod gpu_peek.ko                          # every NVIDIA display function
 *   insmod gpu_peek.ko target=0000:01:00.0      # one card
 *   insmod gpu_peek.ko target=... cluster=1     # + the 31-word speed-select cluster
 *   insmod gpu_peek.ko target=... cluster=1 permissions=1  # + AD102 PLM targets
 *   dmesg | grep gpu_peek
 *
 * cluster=1 adds the fuse/FEAT_OVR/FEAT_READOUT words from the register report and decodes
 * FEAT_READOUT_1 (nine 3-bit per-unit values, 0 = full) and the override nibbles.
 * It is still one ioread32 per word: nothing here writes the card.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

/* Values gpu_unlock_610.c compares against. */
#define GU_SS0_UNLOCK 0x88888888u
#define GU_SS1_UNLOCK 0x8u
#define GU_GFW_BOOT_COMPLETED 0xffu
#define GU_GFW_CPUCTL_HALTED 0x10u
#define GU_FALCON_BAD 0xbadf0000u

static char *target;
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "PCI address to sample, e.g. 0000:01:00.0 (default: every NVIDIA display function)");

static bool cluster;
module_param(cluster, bool, 0444);
MODULE_PARM_DESC(cluster, "also dump the fuse/FEAT_OVR/FEAT_READOUT speed-select cluster (read-only)");

static bool permissions;
module_param(permissions, bool, 0444);
MODULE_PARM_DESC(permissions, "also sample the three additional AD102 payload permission targets (read-only)");

static bool prom;
module_param(prom, bool, 0444);
MODULE_PARM_DESC(prom, "also sample PROM at BAR0 + 0x300000 and parse FWSEC (read-only)");

static bool sec2 = true;
module_param(sec2, bool, 0444);
MODULE_PARM_DESC(sec2, "sample SEC2 Falcon and BCR registers (read-only)");

#include "../../kernel/gpu_unlock/vbios_610.c"

enum {
	S_PMC, S_GFW_PLM, S_GFW_BOOT, S_GFW_CPUCTL, S_WPR2_LO, S_WPR2_HI,
	S_PLM_ENTRY, S_FEATURE_READOUT, S_SS0, S_SS1, S_COUNT
};

struct probe {
	const char *name;
	u32 offset;
};

/* Order must match the enum above. */
static const struct probe probes[S_COUNT] = {
	[S_PMC]             = { "PMC_BOOT_0 (chip id)",     0x00000000 },
	[S_GFW_PLM]         = { "GFW PLM",                  0x00118128 },
	[S_GFW_BOOT]        = { "GFW boot progress",        0x00118234 },
	[S_GFW_CPUCTL]      = { "GFW falcon CPUCTL",        0x00110100 },
	[S_WPR2_LO]         = { "WPR2 lo",                  0x001fa824 },
	[S_WPR2_HI]         = { "WPR2 hi",                  0x001fa828 },
	[S_PLM_ENTRY]       = { "PLM entry",                0x00823804 },
	[S_FEATURE_READOUT] = { "fuse feature_readout",     0x00823814 },
	[S_SS0]             = { "SS0 speed select",         0x0082381c },
	[S_SS1]             = { "SS1 speed select",         0x00823820 },
};

/*
 * The speed-select cluster. Names and field widths are from the GA100/GA102
 * reference table the CMP 170HX work published (the 0x00820000 fuse block and
 * the 0x008238xx FEAT_OVR/FEAT_READOUT window), which matches the offsets this
 * report already uses. FEAT_READOUT_1 is the register that says what the SM
 * speed selectors are *effectively* set to; FEAT_OVR_SM_SPEED_SELECT is the
 * override the unlock writes. Both are read here so an unlock has a before/after
 * that needs no driver and no benchmark.
 */
struct cluster_reg {
	const char *name;
	u32 offset;
};

static const struct cluster_reg cluster_regs[] = {
	{ "FUSECTRL",                        0x00820000 },
	{ "EN_SW_OVERRIDE",                  0x00820040 },
	{ "ENABLE_FUSE_PROGRAM_STATUS",      0x00820078 },
	{ "DISABLE_FUSE_PROGRAM",            0x0082007c },
	{ "DISABLE_SW_OVERRIDE_STATUS",      0x00820084 },
	{ "FUSE_SS_PLM (speed-select PLM)",  0x008200fc },
	{ "FUSE_SS_DP",                      0x00820224 },
	{ "SECURE_FEAT_OVR_QUADRO_WR_SEC",   0x0082038c },
	{ "FUSE_SS_FFMA",                    0x0082059c },
	{ "FUSE_SS_FMLA16",                  0x008207d4 },
	{ "FUSE_SS_FMLA32",                  0x008207d8 },
	{ "FUSE_SS_IMLA0",                   0x008207dc },
	{ "FUSE_SS_IMLA1",                   0x008207e0 },
	{ "FUSE_SS_IMLA2",                   0x008207e4 },
	{ "FUSE_SS_IMLA3",                   0x008207e8 },
	{ "FUSE_SS_IMLA4",                   0x008207ec },
	{ "FEAT_OVR_ECC_PLM",                0x00823800 },
	{ "FEAT_OVR_PLM",                    0x00823804 },
	{ "FEAT_OVR_QUADRO",                 0x00823808 },
	{ "FEAT_OVR_ECC",                    0x0082380c },
	{ "FEAT_OVR_ECC_1",                  0x00823810 },
	{ "FEAT_READOUT_0",                  0x00823814 },
	{ "FEAT_READOUT_1 (SS effective)",   0x00823818 },
	{ "FEAT_OVR_SM_SPEED_SELECT",        0x0082381c },
	{ "FEAT_OVR_SM_SPEED_SELECT_1",      0x00823820 },
	{ "FEAT_OVR_ROW_REMAPPER",           0x00823824 },
	{ "FEAT_READOUT_2",                  0x00823828 },
	{ "FEAT_OVR_ECC_2",                  0x0082382c },
	{ "FECS_FEAT_OVERRIDE_SM_SPEED_SEL", 0x00409664 },
	{ "FECS_FEAT_READOUT_1",             0x00409668 },
	{ "SM_ISSUE_RATE_MODIFIER",          0x00504204 },
	{ "GU_FUSE_SEC2_UCODE1",             0x00824148 },
	{ "FWSEC fuse (ucode id 9)",         0x008241e0 },
};

static void peek_cluster(struct pci_dev *pdev, void __iomem *bar)
{
	unsigned int i, field;
	char decoded[64];
	int build;
	u32 value;

	for (i = 0; i < ARRAY_SIZE(cluster_regs); i++) {
		value = ioread32(bar + cluster_regs[i].offset);
		dev_info(&pdev->dev, "gpu_peek: %#010x %-34s = %#010x\n",
			 cluster_regs[i].offset, cluster_regs[i].name, value);
	}

	/* Nine 3-bit per-unit values: 0 = full rate, 1..5 = reduced (fuse doc). */
	value = ioread32(bar + 0x00823818);
	build = 0;
	for (field = 0; field < 9; field++)
		build += scnprintf(decoded + build, sizeof(decoded) - build, " %u",
				   (value >> (3 * field)) & 7u);
	dev_info(&pdev->dev, "gpu_peek: FEAT_READOUT_1 units (3-bit, 0=full):%s\n", decoded);

	/* Override words: one nibble per unit, bit 3 = enable, bits 2:0 = value. */
	value = ioread32(bar + 0x0082381c);
	build = 0;
	for (field = 0; field < 8; field++)
		build += scnprintf(decoded + build, sizeof(decoded) - build, " %x",
				   (value >> (4 * field)) & 0xfu);
	dev_info(&pdev->dev, "gpu_peek: FEAT_OVR_SM_SPEED_SELECT nibbles:%s\n", decoded);
}

static void peek_one(struct pci_dev *pdev)
{
	u32 value[S_COUNT];
	void __iomem *bar;
	unsigned int i;
	u16 command = 0xffff;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		dev_info(&pdev->dev, "gpu_peek: BAR0 is not memory, skipped\n");
		return;
	}
	if (!pci_resource_start(pdev, 0)) {
		dev_info(&pdev->dev, "gpu_peek: BAR0 not assigned, skipped\n");
		return;
	}
	bar = pci_iomap(pdev, 0, 0);
	if (!bar) {
		dev_info(&pdev->dev, "gpu_peek: BAR0 map failed, skipped\n");
		return;
	}
	pci_read_config_word(pdev, PCI_COMMAND, &command);
	dev_info(&pdev->dev,
		 "gpu_peek: driver=%s BAR0=%#llx len=%#llx command=%#x (MEM=0x%x MASTER=0x%x)\n",
		 pdev->driver ? pdev->driver->name : "none",
		 (unsigned long long)pci_resource_start(pdev, 0),
		 (unsigned long long)pci_resource_len(pdev, 0), command,
		 !!(command & PCI_COMMAND_MEMORY), !!(command & PCI_COMMAND_MASTER));

	for (i = 0; i < S_COUNT; i++) {
		value[i] = ioread32(bar + probes[i].offset);
		dev_info(&pdev->dev, "gpu_peek: %#010x %-22s = %#010x\n",
			 probes[i].offset, probes[i].name, value[i]);
	}
	if (cluster)
		peek_cluster(pdev, bar);
	if (permissions) {
		static const struct probe permission_regs[] = {
			{ "WPR_CFG PLM", 0x001fa7cc },
			{ "FBPA PLM",    0x009a0148 },
			{ "WPR PLM",     0x001fa7c4 },
		};

		for (i = 0; i < ARRAY_SIZE(permission_regs); i++)
			dev_info(&pdev->dev, "gpu_peek: %#010x %-22s = %#010x\n",
				 permission_regs[i].offset, permission_regs[i].name,
				 ioread32(bar + permission_regs[i].offset));
	}
	if (sec2) {
		static const struct probe sec2_regs[] = {
			{ "SEC2 HWCFG",     0x00840108 },
			{ "SEC2 CPUCTL",    0x00840100 },
			{ "SEC2 MAILBOX0",  0x00840040 },
			{ "SEC2 MAILBOX1",  0x00840044 },
			{ "SEC2 RESET",     0x008403c0 },
			{ "SEC2 RESET PLM", 0x008403c4 },
			{ "SEC2 BCR_CTRL",  0x00841668 },
			{ "SEC2 DMACTL",    0x0084010c },
		};

		for (i = 0; i < ARRAY_SIZE(sec2_regs); i++)
			dev_info(&pdev->dev, "gpu_peek: %#010x %-22s = %#010x\n",
				 sec2_regs[i].offset, sec2_regs[i].name,
				 ioread32(bar + sec2_regs[i].offset));
	}
	if (prom) {
		size_t bar_len = pci_resource_len(pdev, 0);
		if (bar_len >= 0x300000 + 0x1a) {
			size_t mapped = min_t(size_t, 1024 * 1024, bar_len - 0x300000);
			u8 *rom_buf = kvmalloc(mapped, GFP_KERNEL);
			if (rom_buf) {
				struct gpu_unlock_fwsec fw;
				int ret;
				memcpy_fromio(rom_buf, bar + 0x300000, mapped);
				dev_info(&pdev->dev, "gpu_peek: PROM @ 0x300000 bytes=%zu sig=%#06x first16=%*ph\n",
					 mapped, (unsigned int)(rom_buf[0] | (rom_buf[1] << 8)),
					 16, rom_buf);
				ret = gpu_unlock_vbios_fwsec(rom_buf, mapped, &fw);
				if (!ret) {
					dev_info(&pdev->dev, "gpu_peek: SUCCESS: FWSEC-FRTS parsed from PROM! ucode_id=%u engine_mask=%#x img_off=%#x img_size=%#x imem=%#x dmem=%#x sig_count=%u sig_versions=%#x\n",
						 fw.ucode_id, fw.engine_mask, fw.image_off, fw.image_size,
						 fw.imem_size, fw.dmem_size, fw.sig_count, fw.sig_versions);
				} else {
					dev_warn(&pdev->dev, "gpu_peek: gpu_unlock_vbios_fwsec failed: %d\n", ret);
				}
				kvfree(rom_buf);
			} else {
				dev_err(&pdev->dev, "gpu_peek: kvmalloc failed for PROM buffer\n");
			}
		} else {
			dev_warn(&pdev->dev, "gpu_peek: BAR0 length %#zx too short for PROM\n", bar_len);
		}
	}
	pci_iounmap(pdev, bar);

	/* The same predicates unlock_610.c uses to judge the card's state. */
	dev_info(&pdev->dev,
		 "gpu_peek: summary boot_completed=%u gfw_plm_level0=%u falcon_halted=%u falcon_accessible=%u wpr2_up=%u plm_open=%u ss0_unlocked=%u ss1_unlocked=%u\n",
		 (value[S_GFW_BOOT] & 0xffu) == GU_GFW_BOOT_COMPLETED,
		 value[S_GFW_PLM] & 1u,
		 !!(value[S_GFW_CPUCTL] & GU_GFW_CPUCTL_HALTED),
		 (value[S_GFW_CPUCTL] & 0xffff0000u) != GU_FALCON_BAD,
		 (value[S_WPR2_HI] >> 4) != 0,
		 value[S_PLM_ENTRY] == 0xffffffffu,
		 value[S_SS0] == GU_SS0_UNLOCK,
		 value[S_SS1] == GU_SS1_UNLOCK);
}

static int __init gpu_peek_init(void)
{
	struct pci_dev *pdev = NULL;

	if (target && *target) {
		unsigned int domain, bus, slot, func;

		if (sscanf(target, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4 ||
		    slot > 0x1f || func > 7) {
			pr_err("gpu_peek: bad target \"%s\"\n", target);
			return -EINVAL;
		}
		pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
		if (!pdev) {
			pr_err("gpu_peek: no device at \"%s\"\n", target);
			return -ENODEV;
		}
		peek_one(pdev);
		pci_dev_put(pdev);
		return 0;
	}
	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		if ((pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY)
			peek_one(pdev);
	return 0;
}

static void __exit gpu_peek_exit(void) { }

module_init(gpu_peek_init);
module_exit(gpu_peek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only sampler for the gpu_unlock probe registers");
MODULE_VERSION("0.1.0");
