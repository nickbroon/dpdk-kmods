/*-
 * GPL LICENSE SUMMARY
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *   The full GNU General Public License is included in this distribution
 *   in the file called LICENSE.GPL.
 *
 *   Contact Information:
 *   Intel Corporation
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/msi.h>
#include <linux/version.h>
#include <linux/slab.h>

#include <rte_pci_dev_features.h>

#include "compat.h"

/**
 * A structure describing the private information for a uio device.
 */
struct rte_uio_pci_dev {
	struct uio_info info;
	struct pci_dev *pdev;
	enum rte_intr_mode mode;
};

static char *intr_mode;
static enum rte_intr_mode igbuio_intr_mode_preferred = RTE_INTR_MODE_MSIX;

/* sriov sysfs */
static ssize_t
show_max_vfs(struct device *dev, struct device_attribute *attr,
	     char *buf)
{
	return snprintf(buf, 10, "%u\n", dev_num_vf(dev));
}

static ssize_t
store_max_vfs(struct device *dev, struct device_attribute *attr,
	      const char *buf, size_t count)
{
	int err = 0;
	unsigned long max_vfs;
	struct pci_dev *pdev = to_pci_dev(dev);

	if (0 != kstrtoul(buf, 0, &max_vfs))
		return -EINVAL;

	if (0 == max_vfs)
		pci_disable_sriov(pdev);
	else if (0 == pci_num_vf(pdev))
		err = pci_enable_sriov(pdev, max_vfs);
	else /* do nothing if change max_vfs number */
		err = -EINVAL;

	return err ? err : count;
}

static DEVICE_ATTR(max_vfs, S_IRUGO | S_IWUSR, show_max_vfs, store_max_vfs);

static struct attribute *dev_attrs[] = {
	&dev_attr_max_vfs.attr,
	NULL,
};

static const struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};

/**
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 *
 * @return
 *  - On success, 0.
 *  - On failure, a negative value.
 */
static int
igbuio_pci_irqcontrol(struct uio_info *info, s32 irq_state)
{
	struct rte_uio_pci_dev *udev = info->priv;
	struct pci_dev *pdev = udev->pdev;

#ifdef HAVE_IRQ_DATA
	struct irq_data *irq = irq_get_irq_data(udev->info.irq);
#else
	unsigned int irq = udev->info.irq;
#endif

	pci_cfg_access_lock(pdev);

	if (udev->mode == RTE_INTR_MODE_MSIX || udev->mode == RTE_INTR_MODE_MSI) {
#ifdef HAVE_PCI_MSI_MASK_IRQ
		if (irq_state == 1)
			pci_msi_unmask_irq(irq);
		else
			pci_msi_mask_irq(irq);
#else
		if (irq_state == 1)
			unmask_msi_irq(irq);
		else
			mask_msi_irq(irq);
#endif
	}

	if (udev->mode == RTE_INTR_MODE_LEGACY)
		pci_intx(pdev, !!irq_state);

	pci_cfg_access_unlock(pdev);

	return 0;
}

/**
 * This is interrupt handler which will check if the interrupt is for the right device.
 * If yes, disable it here and will be enable later.
 */
static irqreturn_t
igbuio_pci_irqhandler(int irq, struct uio_info *info)
{
	struct rte_uio_pci_dev *udev = info->priv;

	/* Legacy mode need to mask in hardware */
	if (udev->mode == RTE_INTR_MODE_LEGACY &&
	    !pci_check_and_mask_intx(udev->pdev))
		return IRQ_NONE;

	/* Message signal mode, no share IRQ and automasked */
	return IRQ_HANDLED;
}

/**
 * This gets called while opening uio device file.
 */
static int
igbuio_pci_open(struct uio_info *info, struct inode *inode)
{
	struct rte_uio_pci_dev *udev = info->priv;
	struct pci_dev *dev = udev->pdev;

	pci_reset_function(dev);

	/* set bus master, which was cleared by the reset function */
	pci_set_master(dev);

	return 0;
}

static int
igbuio_pci_release(struct uio_info *info, struct inode *inode)
{
	struct rte_uio_pci_dev *udev = info->priv;
	struct pci_dev *dev = udev->pdev;

	/* stop the device from further DMA */
	pci_clear_master(dev);

	pci_reset_function(dev);

	return 0;
}

/* Remap pci resources described by bar #pci_bar in uio resource n. */
static int
igbuio_pci_setup_iomem(struct pci_dev *dev, struct uio_info *info,
		       int n, int pci_bar, const char *name)
{
	unsigned long addr, len;
	void *internal_addr;

	if (n >= ARRAY_SIZE(info->mem))
		return -EINVAL;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -1;
	internal_addr = ioremap(addr, len);
	if (internal_addr == NULL)
		return -1;
	info->mem[n].name = name;
	info->mem[n].addr = addr;
	info->mem[n].internal_addr = internal_addr;
	info->mem[n].size = len;
	info->mem[n].memtype = UIO_MEM_PHYS;
	return 0;
}

/* Get pci port io resources described by bar #pci_bar in uio resource n. */
static int
igbuio_pci_setup_ioport(struct pci_dev *dev, struct uio_info *info,
		int n, int pci_bar, const char *name)
{
	unsigned long addr, len;

	if (n >= ARRAY_SIZE(info->port))
		return -EINVAL;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -EINVAL;

	info->port[n].name = name;
	info->port[n].start = addr;
	info->port[n].size = len;
	info->port[n].porttype = UIO_PORT_X86;

	return 0;
}

/* Unmap previously ioremap'd resources */
static void
igbuio_pci_release_iomem(struct uio_info *info)
{
	int i;

	for (i = 0; i < MAX_UIO_MAPS; i++) {
		if (info->mem[i].internal_addr)
			iounmap(info->mem[i].internal_addr);
	}
}

static int
igbuio_pci_enable_interrupts(struct rte_uio_pci_dev *udev)
{
	int err = 0;
#ifndef HAVE_ALLOC_IRQ_VECTORS
	struct msix_entry msix_entry;
#endif

	switch (igbuio_intr_mode_preferred) {
	case RTE_INTR_MODE_MSIX:
		/* Only 1 msi-x vector needed */
#ifndef HAVE_ALLOC_IRQ_VECTORS
		msix_entry.entry = 0;
		if (pci_enable_msix(udev->pdev, &msix_entry, 1) == 0) {
			dev_dbg(&udev->pdev->dev, "using MSI-X");
			udev->info.irq_flags = IRQF_NO_THREAD;
			udev->info.irq = msix_entry.vector;
			udev->mode = RTE_INTR_MODE_MSIX;
			break;
		}
#else
		if (pci_alloc_irq_vectors(udev->pdev, 1, 1, PCI_IRQ_MSIX) == 1) {
			dev_dbg(&udev->pdev->dev, "using MSI-X");
			udev->info.irq_flags = IRQF_NO_THREAD;
			udev->info.irq = pci_irq_vector(udev->pdev, 0);
			udev->mode = RTE_INTR_MODE_MSIX;
			break;
		}
#endif
	/* fall back to MSI */
	case RTE_INTR_MODE_MSI:
#ifndef HAVE_ALLOC_IRQ_VECTORS
		if (pci_enable_msi(udev->pdev) == 0) {
			dev_dbg(&udev->pdev->dev, "using MSI");
			udev->info.irq_flags = IRQF_NO_THREAD;
			udev->info.irq = udev->pdev->irq;
			udev->mode = RTE_INTR_MODE_MSI;
			break;
		}
#else
		if (pci_alloc_irq_vectors(udev->pdev, 1, 1, PCI_IRQ_MSI) == 1) {
			dev_dbg(&udev->pdev->dev, "using MSI");
			udev->info.irq_flags = IRQF_NO_THREAD;
			udev->info.irq = pci_irq_vector(udev->pdev, 0);
			udev->mode = RTE_INTR_MODE_MSI;
			break;
		}
#endif
	/* fall back to INTX */
	case RTE_INTR_MODE_LEGACY:
		if (pci_intx_mask_supported(udev->pdev)) {
			dev_dbg(&udev->pdev->dev, "using INTX");
			udev->info.irq_flags = IRQF_SHARED | IRQF_NO_THREAD;
			udev->info.irq = udev->pdev->irq;
			udev->mode = RTE_INTR_MODE_LEGACY;
			break;
		}
		dev_notice(&udev->pdev->dev, "PCI INTX mask not supported\n");
	/* fall back to no IRQ */
	case RTE_INTR_MODE_NONE:
		udev->mode = RTE_INTR_MODE_NONE;
		udev->info.irq = UIO_IRQ_NONE;
		break;

	default:
		dev_err(&udev->pdev->dev, "invalid IRQ mode %u",
			igbuio_intr_mode_preferred);
		err = -EINVAL;
	}

	return err;
}

static void
igbuio_pci_disable_interrupts(struct rte_uio_pci_dev *udev)
{
#ifndef HAVE_ALLOC_IRQ_VECTORS
	if (udev->mode == RTE_INTR_MODE_MSIX)
		pci_disable_msix(udev->pdev);
	if (udev->mode == RTE_INTR_MODE_MSI)
		pci_disable_msi(udev->pdev);
#else
	if (udev->mode == RTE_INTR_MODE_MSIX ||
	    udev->mode == RTE_INTR_MODE_MSI)
		pci_free_irq_vectors(udev->pdev);
#endif
}

static int
igbuio_setup_bars(struct pci_dev *dev, struct uio_info *info)
{
	int i, iom, iop, ret;
	unsigned long flags;
	static const char *bar_names[PCI_STD_RESOURCE_END + 1]  = {
		"BAR0",
		"BAR1",
		"BAR2",
		"BAR3",
		"BAR4",
		"BAR5",
	};

	iom = 0;
	iop = 0;

	for (i = 0; i < ARRAY_SIZE(bar_names); i++) {
		if (pci_resource_len(dev, i) != 0 &&
				pci_resource_start(dev, i) != 0) {
			flags = pci_resource_flags(dev, i);
			if (flags & IORESOURCE_MEM) {
				ret = igbuio_pci_setup_iomem(dev, info, iom,
							     i, bar_names[i]);
				if (ret != 0)
					return ret;
				iom++;
			} else if (flags & IORESOURCE_IO) {
				ret = igbuio_pci_setup_ioport(dev, info, iop,
							      i, bar_names[i]);
				if (ret != 0)
					return ret;
				iop++;
			}
		}
	}

	return (iom != 0 || iop != 0) ? ret : -ENOENT;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
static int __devinit
#else
static int
#endif
igbuio_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct rte_uio_pci_dev *udev;
	dma_addr_t map_dma_addr;
	void *map_addr;
	int err;

	udev = kzalloc(sizeof(struct rte_uio_pci_dev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	/*
	 * enable device: ask low-level code to enable I/O and
	 * memory
	 */
	err = pci_enable_device(dev);
	if (err != 0) {
		dev_err(&dev->dev, "Cannot enable PCI device\n");
		goto fail_free;
	}

	/* enable bus mastering on the device */
	pci_set_master(dev);

	/* remap IO memory */
	err = igbuio_setup_bars(dev, &udev->info);
	if (err != 0)
		goto fail_release_iomem;

	/* set 64-bit DMA mask */
	err = pci_set_dma_mask(dev,  DMA_BIT_MASK(64));
	if (err != 0) {
		dev_err(&dev->dev, "Cannot set DMA mask\n");
		goto fail_release_iomem;
	}

	err = pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(64));
	if (err != 0) {
		dev_err(&dev->dev, "Cannot set consistent DMA mask\n");
		goto fail_release_iomem;
	}

	/* fill uio infos */
	udev->info.name = "igb_uio";
	udev->info.version = "0.1";
	udev->info.handler = igbuio_pci_irqhandler;
	udev->info.irqcontrol = igbuio_pci_irqcontrol;
	udev->info.open = igbuio_pci_open;
	udev->info.release = igbuio_pci_release;
	udev->info.priv = udev;
	udev->pdev = dev;

	err = igbuio_pci_enable_interrupts(udev);
	if (err != 0)
		goto fail_release_iomem;

	err = sysfs_create_group(&dev->dev.kobj, &dev_attr_grp);
	if (err != 0)
		goto fail_disable_interrupts;

	/* register uio driver */
	err = uio_register_device(&dev->dev, &udev->info);
	if (err != 0)
		goto fail_remove_group;

	pci_set_drvdata(dev, udev);

	dev_info(&dev->dev, "uio device registered with irq %lx\n",
		 udev->info.irq);

	/*
	 * Doing a harmless dma mapping for attaching the device to
	 * the iommu identity mapping if kernel boots with iommu=pt.
	 * Note this is not a problem if no IOMMU at all.
	 */
	map_addr = dma_alloc_coherent(&dev->dev, 1024, &map_dma_addr,
			GFP_KERNEL);
	if (map_addr)
		memset(map_addr, 0, 1024);

	if (!map_addr)
		dev_info(&dev->dev, "dma mapping failed\n");
	else {
		dev_info(&dev->dev, "mapping 1K dma=%#llx host=%p\n",
			 (unsigned long long)map_dma_addr, map_addr);

		dma_free_coherent(&dev->dev, 1024, map_addr, map_dma_addr);
		dev_info(&dev->dev, "unmapping 1K dma=%#llx host=%p\n",
			 (unsigned long long)map_dma_addr, map_addr);
	}

	return 0;

fail_remove_group:
	sysfs_remove_group(&dev->dev.kobj, &dev_attr_grp);
fail_disable_interrupts:
	igbuio_pci_disable_interrupts(udev);
fail_release_iomem:
	igbuio_pci_release_iomem(&udev->info);
	pci_disable_device(dev);
fail_free:
	kfree(udev);

	return err;
}

static void
igbuio_pci_remove(struct pci_dev *dev)
{
	struct rte_uio_pci_dev *udev = pci_get_drvdata(dev);

	sysfs_remove_group(&dev->dev.kobj, &dev_attr_grp);
	uio_unregister_device(&udev->info);
	igbuio_pci_disable_interrupts(udev);
	igbuio_pci_release_iomem(&udev->info);
	pci_disable_device(dev);
	pci_set_drvdata(dev, NULL);
	kfree(udev);
}

static int
igbuio_config_intr_mode(char *intr_str)
{
	if (!intr_str) {
		pr_info("Use MSIX interrupt by default\n");
		return 0;
	}

	if (!strcmp(intr_str, RTE_INTR_MODE_MSIX_NAME)) {
		igbuio_intr_mode_preferred = RTE_INTR_MODE_MSIX;
		pr_info("Use MSIX interrupt\n");
	} else if (!strcmp(intr_str, RTE_INTR_MODE_MSI_NAME)) {
		igbuio_intr_mode_preferred = RTE_INTR_MODE_MSI;
		pr_info("Use MSI interrupt\n");
	} else if (!strcmp(intr_str, RTE_INTR_MODE_LEGACY_NAME)) {
		igbuio_intr_mode_preferred = RTE_INTR_MODE_LEGACY;
		pr_info("Use legacy interrupt\n");
	} else {
		pr_info("Error: bad parameter - %s\n", intr_str);
		return -EINVAL;
	}

	return 0;
}

static struct pci_driver igbuio_pci_driver = {
	.name = "igb_uio",
	.id_table = NULL,
	.probe = igbuio_pci_probe,
	.remove = igbuio_pci_remove,
};

static int __init
igbuio_pci_init_module(void)
{
	int ret;

	ret = igbuio_config_intr_mode(intr_mode);
	if (ret < 0)
		return ret;

	return pci_register_driver(&igbuio_pci_driver);
}

static void __exit
igbuio_pci_exit_module(void)
{
	pci_unregister_driver(&igbuio_pci_driver);
}

module_init(igbuio_pci_init_module);
module_exit(igbuio_pci_exit_module);

module_param(intr_mode, charp, S_IRUGO);
MODULE_PARM_DESC(intr_mode,
"igb_uio interrupt mode (default=msix):\n"
"    " RTE_INTR_MODE_MSIX_NAME "       Use MSIX interrupt\n"
"    " RTE_INTR_MODE_MSI_NAME "        Use MSI interrupt\n"
"    " RTE_INTR_MODE_LEGACY_NAME "     Use Legacy interrupt\n"
"\n");

MODULE_DESCRIPTION("UIO driver for Intel IGB PCI cards");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
