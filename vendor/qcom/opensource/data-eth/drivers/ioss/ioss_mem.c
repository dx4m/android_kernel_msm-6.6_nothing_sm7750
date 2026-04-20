/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/iommu.h>
#include <linux/qcom-iommu-util.h>

#include "ioss_i.h"

static void *default_mem_alloc(struct ioss_device *idev,
		size_t size, dma_addr_t *daddr,
		gfp_t gfp, struct ioss_mem_allocator *alctr)
{
	return dma_alloc_coherent(ioss_idev_to_real(idev), size, daddr, gfp);
}

static void default_mem_free(struct ioss_device *idev,
		size_t size, void *addr, dma_addr_t daddr,
		struct ioss_mem_allocator *alctr)
{
	dma_free_coherent(ioss_idev_to_real(idev), size, addr, daddr);
}

static phys_addr_t default_mem_pa(struct ioss_device *idev,
		void *addr, dma_addr_t daddr,
		struct ioss_mem_allocator *alctr)
{
	return page_to_phys(
		vmalloc_to_page(addr)) | ((phys_addr_t)addr & ~PAGE_MASK);
}

struct ioss_mem_allocator ioss_default_alctr = {
	.name = "dma_alloc_coherent",
	.alloc = default_mem_alloc,
	.free = default_mem_free,
	.pa = default_mem_pa,
};

#ifdef LLCC_ENABLE
/* LLCC Memory Allocator */
#include <linux/soc/qcom/llcc-qcom.h>
#include <linux/dma-map-ops.h>

bool tcm_in_use;
static struct llcc_tcm_data *tcm_mem;

static void *llcc_mem_alloc(struct ioss_device *idev,
		size_t size, dma_addr_t *daddr,
		gfp_t gfp, struct ioss_mem_allocator *alctr)
{
	int rc;
	dma_addr_t iova;
	int prot = IOMMU_READ | IOMMU_WRITE;
	struct device *dev = ioss_idev_to_real(idev);
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	if (!tcm_mem || tcm_in_use || size > tcm_mem->mem_size)
		return NULL;

	/* If SMMU S1 bypass is enabled, we do not have to map/remap TCM to device. */
	rc = qcom_iommu_get_mappings_configuration(domain);
	if ((rc > 0) && (rc & QCOM_IOMMU_MAPPING_CONF_S1_BYPASS)) {
		ioss_dev_log(idev, "SMMU is in S1 bypass. Skipping TCM DMA mapping.");

		tcm_in_use = true;
		*daddr = tcm_mem->phys_addr;
		return tcm_mem->virt_addr;
	}

	/* Use dma_map_resource() to map TCM so that a valid IOVA is allocated
	 * iommu_unmap followed byiommu_map is used to map TCM memory as normal memory
	 * from device memory.
	 */
	iova = dma_map_resource(dev, tcm_mem->phys_addr, tcm_mem->mem_size,
						DMA_BIDIRECTIONAL, 0);

	if (dma_mapping_error(dev, iova)) {
		ioss_dev_err(idev, "DMA map of TCM failed");
		return NULL;
	}

	ioss_dev_log(idev, "DMA map of TCM succeeded, using %pad as IOVA", &iova);

	if(tcm_mem->mem_size != iommu_unmap(domain, iova, tcm_mem->mem_size)) {
		ioss_dev_err(idev, "IOMMU unmap of TCM failed");
		dma_unmap_resource(dev,
			tcm_mem->phys_addr, tcm_mem->mem_size, DMA_BIDIRECTIONAL, 0);
		return NULL;
	}

	if (dev_is_dma_coherent(dev))
		prot |= IOMMU_CACHE;

	rc = iommu_map(domain, iova,
			tcm_mem->phys_addr, tcm_mem->mem_size, prot, gfp);
	if (rc) {
		ioss_dev_err(idev, "Failed to remap TCM as normal memory");
		dma_unmap_resource(dev,
			tcm_mem->phys_addr, tcm_mem->mem_size, DMA_BIDIRECTIONAL, 0);
		return NULL;
	} else {
		ioss_dev_log(idev, "Successfully remapped TCM as normal memory");
	}

	*daddr = iova;

	tcm_in_use = true;

	return tcm_mem->virt_addr;
}

static void llcc_mem_free(struct ioss_device *idev,
		size_t size, void *addr, dma_addr_t daddr,
		struct ioss_mem_allocator *alctr)
{
	dma_unmap_resource(ioss_idev_to_real(idev),
			daddr, size, DMA_BIDIRECTIONAL, 0);

	tcm_in_use = false;
}

static phys_addr_t llcc_mem_pa(struct ioss_device *idev,
		void *addr, dma_addr_t daddr,
		struct ioss_mem_allocator *alctr)
{
	return tcm_mem->phys_addr + (tcm_mem->virt_addr - addr);
}

static size_t llcc_mem_get(void)
{
	struct llcc_tcm_data *tcm_data;

	if (tcm_mem) {
		ioss_log_msg(NULL, "TCM MEM already in use");
		return 0;
	}

	tcm_data = llcc_tcm_activate();
	if (IS_ERR_OR_NULL(tcm_data)) {
		ioss_log_err(NULL, "Failed to activate TCM");
		return 0;
	}

	tcm_mem = tcm_data;

	return tcm_mem->mem_size;
}

static void llcc_mem_put(void)
{
	struct llcc_tcm_data *tcm_data = tcm_mem;

	if (!tcm_data)
		return;

	tcm_mem = NULL;

	llcc_tcm_deactivate(tcm_data);
}

struct ioss_mem_allocator ioss_llcc_alctr = {
	.name = "llcc_mem_allocator",
	.alloc = llcc_mem_alloc,
	.free = llcc_mem_free,
	.pa = llcc_mem_pa,
	.get = llcc_mem_get,
	.put = llcc_mem_put,
};
#endif
