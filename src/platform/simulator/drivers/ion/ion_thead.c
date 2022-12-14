/*
 * Copyright (C) 2021 Alibaba Group Holding Limited
 * Author: LuChongzhi <chongzhi.lcz@alibaba-inc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/dma-buf.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>

#include "ion.h"
#define ION_DEBUG

struct ion_data {
	int npages;
	struct page *pages[];
};

#ifdef ION_DEBUG
static void dump_dma_buf(struct dma_buf *dmabuf)
{
	int i;
	struct ion_data *data = dmabuf->priv;

	pr_info("dma_buf(0x%p) = {\n", dmabuf);
	pr_info("\t size=%ld\n", dmabuf->size);
	pr_info("\t priv(0x%p)={\n", data);
	if (data != NULL) {
		pr_info("\t\t data->npages=%d\n", data->npages);
		for (i = 0; i < data->npages; i++) {
			pr_info("\t\t pages[%d]=0x%p\n", i, data->pages[i]);
		}
	}
	pr_info("}\n");
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,4,0)
static int ion_attach(struct dma_buf *dmabuf,
		      struct dma_buf_attachment *attachment)
{
	pr_info("dmabuf attach device: %s\n", dev_name(attachment->dev));
	return 0;
}

#else
static int ion_attach(struct dma_buf *dmabuf,
                      struct device *dev,
                      struct dma_buf_attachment *attachment)
{
   pr_info("dmabuf attach device:%s\n",dev_name(attachment->dev));  
   return 0;	
}

#endif

static void ion_detach(struct dma_buf *dmabuf,
		       struct dma_buf_attachment *attachment)
{
	pr_info("dmabuf detach device: %s\n", dev_name(attachment->dev));
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction dir)
{
	struct ion_data *data = attachment->dmabuf->priv;
	struct sg_table *table;
	struct scatterlist *sg;
	int i;

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(table, data->npages, GFP_KERNEL)) {
		kfree(table);
		return ERR_PTR(-ENOMEM);
	}

	sg = table->sgl;
	for (i = 0; i < data->npages; i++) {
		sg_set_page(sg, data->pages[i], PAGE_SIZE, 0);
		sg = sg_next(sg);
	}

	if (!dma_map_sg(NULL, table->sgl, table->nents, dir)) {
		sg_free_table(table);
		kfree(table);
		return ERR_PTR(-ENOMEM);
	}

	return table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction dir)
{
	dma_unmap_sg(NULL, table->sgl, table->nents, dir);
	sg_free_table(table);
	kfree(table);
}

static void ion_release(struct dma_buf *dma_buf)
{
	struct ion_data *data = dma_buf->priv;
	int i;

	pr_info("dmabuf release\n");

	for (i = 0; i < data->npages; i++)
		put_page(data->pages[i]);

	kfree(data);
}

static void *ion_vmap(struct dma_buf *dma_buf)
{
	struct ion_data *data = dma_buf->priv;

	return vm_map_ram(data->pages, data->npages, 0, PAGE_KERNEL);
}

static void ion_vunmap(struct dma_buf *dma_buf, void *vaddr)
{
	struct ion_data *data = dma_buf->priv;

	vm_unmap_ram(vaddr, data->npages);
}

static void *ion_kmap(struct dma_buf *dma_buf, unsigned long page_num)
{
	struct ion_data *data = dma_buf->priv;

	return kmap(data->pages[page_num]);
}

static void ion_kunmap(struct dma_buf *dma_buf, unsigned long page_num,
		       void *addr)
{
	struct ion_data *data = dma_buf->priv;

	return kunmap(data->pages[page_num]);
}

static int ion_mmap(struct dma_buf *dma_buf, struct vm_area_struct *vma)
{
	struct ion_data *data = dma_buf->priv;
	unsigned long vm_start = vma->vm_start;
	int i;

#ifdef ION_DEBUG
	pr_info("[%s:%d] dma_buf=%p, vma=%p\n", __func__, __LINE__, dma_buf, vma);
	pr_info("[%s:%d] data=%p, vm_start=0x%lx\n", __func__, __LINE__, data, vm_start);
	pr_info("[%s:%d] vma->vm_flags=0x%lx\n", __func__, __LINE__, vma->vm_flags);
	dump_dma_buf(dma_buf);
#endif
	vma->vm_flags |= VM_IO;

	for (i = 0; i < data->npages; i++) {
		remap_pfn_range(vma, vm_start, page_to_pfn(data->pages[i]),
				PAGE_SIZE, vma->vm_page_prot);
		vm_start += PAGE_SIZE;
	}

	pr_info("[%s:%d] vm_start=0x%lx, vm_end=0x%lx\n", __func__, __LINE__,
			vma->vm_start, vma->vm_end);

	return 0;
}

static int ion_begin_cpu_access(struct dma_buf *dmabuf,
				enum dma_data_direction dir)
{
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	if (list_empty(&dmabuf->attachments))
		return 0;

	attachment = list_first_entry(&dmabuf->attachments, struct dma_buf_attachment,
				      node);
	table = attachment->priv;
	dma_sync_sg_for_cpu(NULL, table->sgl, table->nents, dir);

	return 0;
}

static int ion_end_cpu_access(struct dma_buf *dmabuf,
			      enum dma_data_direction dir)
{
	struct dma_buf_attachment *attachment;
	struct sg_table *table;

	if (list_empty(&dmabuf->attachments))
		return 0;

	attachment = list_first_entry(&dmabuf->attachments, struct dma_buf_attachment,
				      node);
	table = attachment->priv;
	dma_sync_sg_for_device(NULL, table->sgl, table->nents, dir);

	return 0;
}

static const struct dma_buf_ops exp_dmabuf_ops = {
	.attach = ion_attach,
	.detach = ion_detach,
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.release = ion_release,
	.map = ion_kmap,
	.unmap = ion_kunmap,
	.mmap = ion_mmap,
	.vmap = ion_vmap,
	.vunmap = ion_vunmap,
	.begin_cpu_access = ion_begin_cpu_access,
	.end_cpu_access = ion_end_cpu_access,
};

static struct dma_buf *ion_alloc(size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	struct ion_data *data;
	int i, npages;
	unsigned long data_size;
#ifdef ION_DEBUG
	char *kvaddr;
#endif

	npages = PAGE_ALIGN(size) / PAGE_SIZE;
	if (!npages)
		return ERR_PTR(-EINVAL);

	data_size = sizeof(*data) + npages * sizeof(struct page *);
	data = kmalloc(data_size, GFP_KERNEL);
	if (!data) {
		pr_err("[%s:%d] kmalloc(%ld) failed\n", __func__, __LINE__, data_size);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < npages; i++) {
		data->pages[i] = alloc_page(GFP_KERNEL);
		if (!data->pages[i])
			goto err;
	}
	data->npages = npages;

	exp_info.ops = &exp_dmabuf_ops;
	exp_info.size = npages * PAGE_SIZE;
	exp_info.flags = O_CLOEXEC | O_RDWR;
	exp_info.priv = data;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("[%s:%d] dma_buf_export failed\n", __func__, __LINE__);
		goto err;
	}

#ifdef ION_DEBUG
	pr_info("[%s:%d] exp_info.size=%ld\n", __func__, __LINE__, exp_info.size);
	kvaddr = ion_vmap(dmabuf);
	pr_info("fill each page header with 'dma-buf page[000i]'\n");
	for (i = 0; i < npages; i++) {
		memset(kvaddr + PAGE_SIZE *i, 0, 32);
		sprintf(kvaddr + PAGE_SIZE *i, "dma-buf page[%04d]", i);
	}
	ion_vunmap(dmabuf, kvaddr);
	dump_dma_buf(dmabuf);
#endif

	return dmabuf;

err:
	pr_err("[%s:%d] err\n", __func__, __LINE__);
	while (i--)
		put_page(data->pages[i]);
	kfree(data);
	return ERR_PTR(-ENOMEM);
}

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct dma_buf *dmabuf;
	struct ion_allocation_data alloc_data;

	if (copy_from_user(&alloc_data, (void __user *)arg, sizeof(alloc_data))) {
		pr_err("ioctl copy_from_user failed!");
		return -EFAULT;
	}

	if (cmd != ION_IOC_ALLOC) {
		pr_err("ioctl %u is not supported!", cmd);
		return -EINVAL;
	}

	dmabuf = ion_alloc(alloc_data.len);
	if (!dmabuf) {
		pr_err("error: exporter alloc page failed\n");
		return -ENOMEM;
	}

	alloc_data.fd = dma_buf_fd(dmabuf, O_CLOEXEC);

	if (copy_to_user((void __user *)arg, &alloc_data, sizeof(alloc_data)))
		return -EFAULT;

	return 0;
}

static struct file_operations ion_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ion_ioctl,
};

static struct miscdevice mdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &ion_fops,
};

static int __init ion_init(void)
{
	pr_info("ion_init()\n");
	return misc_register(&mdev);
}

static void __exit ion_exit(void)
{
	pr_info("ion_exit()\n");
	misc_deregister(&mdev);
}

module_init(ion_init);
module_exit(ion_exit);

MODULE_AUTHOR("LuChongzhi <chongzhi.lcz@alibaba-inc.com>");
MODULE_DESCRIPTION("T-Head ion dma-buf exporter based on continous system heap");
MODULE_LICENSE("GPL v2");
