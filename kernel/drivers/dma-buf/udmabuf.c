// SPDX-License-Identifier: GPL-2.0
/*
 * udmabuf: turn memfd pages into a dma-buf (/dev/udmabuf).
 *
 * Backport of Linux 4.20's drivers/dma-buf/udmabuf.c (Gerd Hoffmann) to
 * 4.14 for the Titan Pocket. libcamera's software ISP allocates its output
 * buffers here when there is no DMA heap (docs/camera.md). Differences from
 * 4.20: 4.14's dma_buf_export() requires the map/map_atomic (kmap) callbacks,
 * the memfd seals come from shmem_get_seals(), fault handlers return int,
 * and the page array is kvmalloc'ed.
 */
#include <linux/dma-buf.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/fcntl.h>
#include <uapi/linux/udmabuf.h>

static int list_limit = 1024;	/* udmabuf_create_list.count limit */
module_param(list_limit, int, 0644);
MODULE_PARM_DESC(list_limit, "udmabuf_create_list->count limit. Default is 1024.");

static int size_limit_mb = 64;	/* total dmabuf size, in megabytes */
module_param(size_limit_mb, int, 0644);
MODULE_PARM_DESC(size_limit_mb, "Max size of a dmabuf, in megabytes. Default is 64.");

struct udmabuf {
	pgoff_t pagecount;
	struct page **pages;
};

static int udmabuf_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct udmabuf *ubuf = vma->vm_private_data;

	if (vmf->pgoff >= ubuf->pagecount)
		return VM_FAULT_SIGBUS;
	vmf->page = ubuf->pages[vmf->pgoff];
	get_page(vmf->page);
	return 0;
}

static const struct vm_operations_struct udmabuf_vm_ops = {
	.fault = udmabuf_vm_fault,
};

static int mmap_udmabuf(struct dma_buf *buf, struct vm_area_struct *vma)
{
	struct udmabuf *ubuf = buf->priv;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	vma->vm_ops = &udmabuf_vm_ops;
	vma->vm_private_data = ubuf;
	return 0;
}

static struct sg_table *map_udmabuf(struct dma_buf_attachment *at,
				    enum dma_data_direction direction)
{
	struct udmabuf *ubuf = at->dmabuf->priv;
	struct sg_table *sg;
	int ret;

	sg = kzalloc(sizeof(*sg), GFP_KERNEL);
	if (!sg)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table_from_pages(sg, ubuf->pages, ubuf->pagecount,
					0, ubuf->pagecount << PAGE_SHIFT,
					GFP_KERNEL);
	if (ret < 0)
		goto err;
	ret = -EINVAL;
	if (!dma_map_sg(at->dev, sg->sgl, sg->nents, direction))
		goto err_free;
	return sg;

err_free:
	sg_free_table(sg);
err:
	kfree(sg);
	return ERR_PTR(ret);
}

static void unmap_udmabuf(struct dma_buf_attachment *at,
			  struct sg_table *sg,
			  enum dma_data_direction direction)
{
	dma_unmap_sg(at->dev, sg->sgl, sg->nents, direction);
	sg_free_table(sg);
	kfree(sg);
}

static void release_udmabuf(struct dma_buf *buf)
{
	struct udmabuf *ubuf = buf->priv;
	pgoff_t pg;

	for (pg = 0; pg < ubuf->pagecount; pg++)
		put_page(ubuf->pages[pg]);
	kvfree(ubuf->pages);
	kfree(ubuf);
}

static void *kmap_udmabuf(struct dma_buf *buf, unsigned long page_num)
{
	struct udmabuf *ubuf = buf->priv;

	return kmap(ubuf->pages[page_num]);
}

static void kunmap_udmabuf(struct dma_buf *buf, unsigned long page_num,
			   void *vaddr)
{
	struct udmabuf *ubuf = buf->priv;

	kunmap(ubuf->pages[page_num]);
}

static void *kmap_atomic_udmabuf(struct dma_buf *buf, unsigned long page_num)
{
	struct udmabuf *ubuf = buf->priv;

	return kmap_atomic(ubuf->pages[page_num]);
}

static void kunmap_atomic_udmabuf(struct dma_buf *buf, unsigned long page_num,
				  void *vaddr)
{
	kunmap_atomic(vaddr);
}

static const struct dma_buf_ops udmabuf_ops = {
	.map_dma_buf	  = map_udmabuf,
	.unmap_dma_buf	  = unmap_udmabuf,
	.release	  = release_udmabuf,
	.map		  = kmap_udmabuf,
	.unmap		  = kunmap_udmabuf,
	.map_atomic	  = kmap_atomic_udmabuf,
	.unmap_atomic	  = kunmap_atomic_udmabuf,
	.mmap		  = mmap_udmabuf,
};

#define SEALS_WANTED (F_SEAL_SHRINK)
#define SEALS_DENIED (F_SEAL_WRITE)

static long udmabuf_create(const struct udmabuf_create_list *head,
			   const struct udmabuf_create_item *list)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct file *memfd = NULL;
	struct udmabuf *ubuf;
	struct dma_buf *buf;
	pgoff_t pgoff, pgcnt, pgidx, pgbuf = 0, pglimit;
	struct page *page;
	int seals, ret = -EINVAL;
	u32 i, flags;

	ubuf = kzalloc(sizeof(*ubuf), GFP_KERNEL);
	if (!ubuf)
		return -ENOMEM;

	pglimit = ((pgoff_t)size_limit_mb * 1024 * 1024) >> PAGE_SHIFT;
	for (i = 0; i < head->count; i++) {
		if (!IS_ALIGNED(list[i].offset, PAGE_SIZE))
			goto err;
		if (!IS_ALIGNED(list[i].size, PAGE_SIZE))
			goto err;
		ubuf->pagecount += list[i].size >> PAGE_SHIFT;
		if (ubuf->pagecount > pglimit)
			goto err;
	}
	if (!ubuf->pagecount)
		goto err;

	ubuf->pages = kvmalloc_array(ubuf->pagecount, sizeof(*ubuf->pages),
				     GFP_KERNEL);
	if (!ubuf->pages) {
		ret = -ENOMEM;
		goto err;
	}

	pgbuf = 0;
	for (i = 0; i < head->count; i++) {
		ret = -EBADFD;
		memfd = fget(list[i].memfd);
		if (!memfd)
			goto err;
		if (!shmem_mapping(file_inode(memfd)->i_mapping))
			goto err;
		seals = shmem_get_seals(memfd);
		if (seals < 0)
			goto err;
		ret = -EINVAL;
		if ((seals & SEALS_WANTED) != SEALS_WANTED ||
		    (seals & SEALS_DENIED) != 0)
			goto err;
		pgoff = list[i].offset >> PAGE_SHIFT;
		pgcnt = list[i].size   >> PAGE_SHIFT;
		for (pgidx = 0; pgidx < pgcnt; pgidx++) {
			page = shmem_read_mapping_page(
				file_inode(memfd)->i_mapping, pgoff + pgidx);
			if (IS_ERR(page)) {
				ret = PTR_ERR(page);
				goto err;
			}
			ubuf->pages[pgbuf++] = page;
		}
		fput(memfd);
		memfd = NULL;
	}

	exp_info.ops  = &udmabuf_ops;
	exp_info.size = ubuf->pagecount << PAGE_SHIFT;
	exp_info.priv = ubuf;
	exp_info.flags = O_RDWR;

	buf = dma_buf_export(&exp_info);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err;
	}

	flags = 0;
	if (head->flags & UDMABUF_FLAGS_CLOEXEC)
		flags |= O_CLOEXEC;
	ret = dma_buf_fd(buf, flags);
	if (ret < 0)
		dma_buf_put(buf);	/* frees ubuf through release_udmabuf */
	return ret;

err:
	while (pgbuf > 0)
		put_page(ubuf->pages[--pgbuf]);
	if (memfd)
		fput(memfd);
	kvfree(ubuf->pages);
	kfree(ubuf);
	return ret;
}

static long udmabuf_ioctl_create(struct file *filp, unsigned long arg)
{
	struct udmabuf_create create;
	struct udmabuf_create_list head;
	struct udmabuf_create_item list;

	if (copy_from_user(&create, (void __user *)arg,
			   sizeof(create)))
		return -EFAULT;

	head.flags  = create.flags;
	head.count  = 1;
	list.memfd  = create.memfd;
	list.offset = create.offset;
	list.size   = create.size;

	return udmabuf_create(&head, &list);
}

static long udmabuf_ioctl_create_list(struct file *filp, unsigned long arg)
{
	struct udmabuf_create_list head;
	struct udmabuf_create_item *list;
	int ret = -EINVAL;
	u32 lsize;

	if (copy_from_user(&head, (void __user *)arg, sizeof(head)))
		return -EFAULT;
	if (head.count > list_limit)
		return -EINVAL;
	lsize = sizeof(struct udmabuf_create_item) * head.count;
	list = memdup_user((void __user *)(arg + sizeof(head)), lsize);
	if (IS_ERR(list))
		return PTR_ERR(list);

	ret = udmabuf_create(&head, list);
	kfree(list);
	return ret;
}

static long udmabuf_ioctl(struct file *filp, unsigned int ioctl,
			  unsigned long arg)
{
	long ret;

	switch (ioctl) {
	case UDMABUF_CREATE:
		ret = udmabuf_ioctl_create(filp, arg);
		break;
	case UDMABUF_CREATE_LIST:
		ret = udmabuf_ioctl_create_list(filp, arg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	return ret;
}

static const struct file_operations udmabuf_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = udmabuf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= udmabuf_ioctl,
#endif
};

static struct miscdevice udmabuf_misc = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "udmabuf",
	.fops           = &udmabuf_fops,
};

static int __init udmabuf_dev_init(void)
{
	return misc_register(&udmabuf_misc);
}

static void __exit udmabuf_dev_exit(void)
{
	misc_deregister(&udmabuf_misc);
}

module_init(udmabuf_dev_init)
module_exit(udmabuf_dev_exit)

MODULE_AUTHOR("Gerd Hoffmann <kraxel@redhat.com>");
MODULE_LICENSE("GPL v2");
