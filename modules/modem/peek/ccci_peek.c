// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only look at the memory LK leaves for the modem driver: the CCCI tag
 * block (the reserved-memory node "ccci_tag_mem", which /chosen's
 * ccci,modem_info_v2 points into), the start of the MD image region and the
 * two AP/MD shared-memory regions. Each is exposed as a debugfs blob under
 * /sys/kernel/debug/ccci_peek/. Nothing is written.
 */
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>

static const struct {
	const char *name;
	phys_addr_t base;
	size_t size;
} regions[] = {
	{ "tag",     0x9d5e0000, 0x10000 },
	{ "nc_smem", 0x88000000, 0x100000 },
	{ "c_smem",  0x86000000, 0x100000 },
};

static struct dentry *dir;
static void *maps[ARRAY_SIZE(regions)];
static struct debugfs_blob_wrapper blobs[ARRAY_SIZE(regions)];

static int __init ccci_peek_init(void)
{
	int i;

	dir = debugfs_create_dir("ccci_peek", NULL);
	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		maps[i] = memremap(regions[i].base, regions[i].size, MEMREMAP_WB);
		if (!maps[i]) {
			pr_warn("ccci_peek: %s: memremap failed\n", regions[i].name);
			continue;
		}
		blobs[i].data = maps[i];
		blobs[i].size = regions[i].size;
		debugfs_create_blob(regions[i].name, 0400, dir, &blobs[i]);
	}
	return 0;
}

static void __exit ccci_peek_exit(void)
{
	int i;

	debugfs_remove_recursive(dir);
	for (i = 0; i < ARRAY_SIZE(regions); i++)
		if (maps[i])
			memunmap(maps[i]);
}

module_init(ccci_peek_init);
module_exit(ccci_peek_exit);
MODULE_LICENSE("GPL");
