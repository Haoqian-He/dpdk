/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include <eal_export.h>
#include <eal_trace_internal.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_string_fns.h>
#include <rte_common.h>

#include "malloc_heap.h"
#include "malloc_elem.h"
#include "eal_private.h"
#include "eal_memcfg.h"

/* Default count used until rte_memzone_max_set() is called */
#define DEFAULT_MAX_MEMZONE_COUNT 2560

RTE_EXPORT_SYMBOL(rte_memzone_max_set)
int
rte_memzone_max_set(size_t max)
{
	struct rte_mem_config *mcfg;

	if (eal_get_internal_configuration()->init_complete > 0) {
		EAL_LOG(ERR, "Max memzone cannot be set after EAL init");
		return -1;
	}

	mcfg = rte_eal_get_configuration()->mem_config;
	if (mcfg == NULL) {
		EAL_LOG(ERR, "Failed to set max memzone count");
		return -1;
	}

	mcfg->max_memzone = max;

	return 0;
}

RTE_EXPORT_SYMBOL(rte_memzone_max_get)
size_t
rte_memzone_max_get(void)
{
	struct rte_mem_config *mcfg;

	mcfg = rte_eal_get_configuration()->mem_config;
	if (mcfg == NULL || mcfg->max_memzone == 0)
		return DEFAULT_MAX_MEMZONE_COUNT;

	return mcfg->max_memzone;
}

static inline const struct rte_memzone *
memzone_lookup_thread_unsafe(const char *name)
{
	struct rte_mem_config *mcfg;
	struct rte_fbarray *arr;
	const struct rte_memzone *mz;
	int i = 0;

	/* get pointer to global configuration */
	mcfg = rte_eal_get_configuration()->mem_config;
	arr = &mcfg->memzones;

	/*
	 * the algorithm is not optimal (linear), but there are few
	 * zones and this function should be called at init only
	 */
	i = rte_fbarray_find_next_used(arr, 0);
	while (i >= 0) {
		mz = rte_fbarray_get(arr, i);
		if (mz->addr != NULL &&
				!strncmp(name, mz->name, RTE_MEMZONE_NAMESIZE))
			return mz;
		i = rte_fbarray_find_next_used(arr, i + 1);
	}
	return NULL;
}

#define MEMZONE_KNOWN_FLAGS (RTE_MEMZONE_2MB \
	| RTE_MEMZONE_1GB \
	| RTE_MEMZONE_16MB \
	| RTE_MEMZONE_16GB \
	| RTE_MEMZONE_256KB \
	| RTE_MEMZONE_256MB \
	| RTE_MEMZONE_512MB \
	| RTE_MEMZONE_4GB \
	| RTE_MEMZONE_SIZE_HINT_ONLY \
	| RTE_MEMZONE_IOVA_CONTIG \
	)

static const struct rte_memzone *
memzone_reserve_aligned_thread_unsafe(const char *name, size_t len,
		int socket_id, unsigned int flags, unsigned int align,
		unsigned int bound)
{
	struct rte_memzone *mz;
	struct rte_mem_config *mcfg;
	struct rte_fbarray *arr;
	void *mz_addr;
	size_t requested_len;
	int mz_idx;
	bool contig;

	/* get pointer to global configuration */
	mcfg = rte_eal_get_configuration()->mem_config;
	arr = &mcfg->memzones;

	/* no more room in config */
	if (arr->count >= arr->len) {
		EAL_LOG(ERR,
		"%s(): Number of requested memzone segments exceeds maximum "
		"%u", __func__, arr->len);

		rte_errno = ENOSPC;
		return NULL;
	}

	if (strlen(name) > sizeof(mz->name) - 1) {
		EAL_LOG(DEBUG, "%s(): memzone <%s>: name too long",
			__func__, name);
		rte_errno = ENAMETOOLONG;
		return NULL;
	}

	/* zone already exist */
	if ((memzone_lookup_thread_unsafe(name)) != NULL) {
		EAL_LOG(DEBUG, "%s(): memzone <%s> already exists",
			__func__, name);
		rte_errno = EEXIST;
		return NULL;
	}

	/* if alignment is not a power of two */
	if (align && !rte_is_power_of_2(align)) {
		EAL_LOG(ERR, "%s(): Invalid alignment: %u", __func__,
				align);
		rte_errno = EINVAL;
		return NULL;
	}

	/* alignment less than cache size is not allowed */
	if (align < RTE_CACHE_LINE_SIZE)
		align = RTE_CACHE_LINE_SIZE;

	/* align length on cache boundary. Check for overflow before doing so */
	if (len > SIZE_MAX - RTE_CACHE_LINE_MASK) {
		rte_errno = EINVAL; /* requested size too big */
		return NULL;
	}

	len = RTE_ALIGN_CEIL(len, RTE_CACHE_LINE_SIZE);

	/* save minimal requested  length */
	requested_len = RTE_MAX((size_t)RTE_CACHE_LINE_SIZE,  len);

	/* check that boundary condition is valid */
	if (bound != 0 && (requested_len > bound || !rte_is_power_of_2(bound))) {
		rte_errno = EINVAL;
		return NULL;
	}

	if ((socket_id != SOCKET_ID_ANY) && socket_id < 0) {
		rte_errno = EINVAL;
		return NULL;
	}

	if ((flags & ~MEMZONE_KNOWN_FLAGS) != 0) {
		rte_errno = EINVAL;
		return NULL;
	}

	/* only set socket to SOCKET_ID_ANY if we aren't allocating for an
	 * external heap.
	 */
	if (!rte_eal_has_hugepages() && socket_id < RTE_MAX_NUMA_NODES)
		socket_id = SOCKET_ID_ANY;

	contig = (flags & RTE_MEMZONE_IOVA_CONTIG) != 0;
	/* malloc only cares about size flags, remove contig flag from flags */
	flags &= ~RTE_MEMZONE_IOVA_CONTIG;

	if (len == 0 && bound == 0) {
		/* no size constraints were placed, so use malloc elem len */
		requested_len = 0;
		mz_addr = malloc_heap_alloc_biggest(socket_id, flags, align, contig);
	} else {
		if (len == 0)
			requested_len = bound;
		/* allocate memory on heap */
		mz_addr = malloc_heap_alloc(requested_len, socket_id, flags, align, bound, contig);
	}
	if (mz_addr == NULL) {
		rte_errno = ENOMEM;
		return NULL;
	}

	struct malloc_elem *elem = malloc_elem_from_data(mz_addr);

	/* fill the zone in config */
	mz_idx = rte_fbarray_find_next_free(arr, 0);

	if (mz_idx < 0) {
		mz = NULL;
	} else {
		rte_fbarray_set_used(arr, mz_idx);
		mz = rte_fbarray_get(arr, mz_idx);
	}

	if (mz == NULL) {
		EAL_LOG(ERR, "%s(): Cannot find free memzone", __func__);
		malloc_heap_free(elem);
		rte_errno = ENOSPC;
		return NULL;
	}

	strlcpy(mz->name, name, sizeof(mz->name));
	mz->iova = rte_malloc_virt2iova(mz_addr);
	mz->addr = mz_addr;
	mz->len = requested_len == 0 ?
			elem->size - elem->pad - MALLOC_ELEM_OVERHEAD :
			requested_len;
	mz->hugepage_sz = elem->msl->page_sz;
	mz->socket_id = elem->msl->socket_id;
	mz->flags = 0;

	return mz;
}

static const struct rte_memzone *
rte_memzone_reserve_thread_safe(const char *name, size_t len, int socket_id,
		unsigned int flags, unsigned int align, unsigned int bound)
{
	struct rte_mem_config *mcfg;
	const struct rte_memzone *mz = NULL;

	/* get pointer to global configuration */
	mcfg = rte_eal_get_configuration()->mem_config;

	rte_rwlock_write_lock(&mcfg->mlock);

	mz = memzone_reserve_aligned_thread_unsafe(
		name, len, socket_id, flags, align, bound);

	rte_eal_trace_memzone_reserve(name, len, socket_id, flags, align,
		bound, mz);

	rte_rwlock_write_unlock(&mcfg->mlock);

	return mz;
}

/*
 * Return a pointer to a correctly filled memzone descriptor (with a
 * specified alignment and boundary). If the allocation cannot be done,
 * return NULL.
 */
RTE_EXPORT_SYMBOL(rte_memzone_reserve_bounded)
const struct rte_memzone *
rte_memzone_reserve_bounded(const char *name, size_t len, int socket_id,
			    unsigned flags, unsigned align, unsigned bound)
{
	return rte_memzone_reserve_thread_safe(name, len, socket_id, flags,
					       align, bound);
}

/*
 * Return a pointer to a correctly filled memzone descriptor (with a
 * specified alignment). If the allocation cannot be done, return NULL.
 */
RTE_EXPORT_SYMBOL(rte_memzone_reserve_aligned)
const struct rte_memzone *
rte_memzone_reserve_aligned(const char *name, size_t len, int socket_id,
			    unsigned flags, unsigned align)
{
	return rte_memzone_reserve_thread_safe(name, len, socket_id, flags,
					       align, 0);
}

/*
 * Return a pointer to a correctly filled memzone descriptor. If the
 * allocation cannot be done, return NULL.
 */
RTE_EXPORT_SYMBOL(rte_memzone_reserve)
const struct rte_memzone *
rte_memzone_reserve(const char *name, size_t len, int socket_id,
		    unsigned flags)
{
	return rte_memzone_reserve_thread_safe(name, len, socket_id,
					       flags, RTE_CACHE_LINE_SIZE, 0);
}

RTE_EXPORT_SYMBOL(rte_memzone_free)
int
rte_memzone_free(const struct rte_memzone *mz)
{
	char name[RTE_MEMZONE_NAMESIZE];
	struct rte_mem_config *mcfg;
	struct rte_fbarray *arr;
	struct rte_memzone *found_mz;
	int ret = 0;
	void *addr = NULL;
	unsigned idx;

	if (mz == NULL)
		return -EINVAL;

	rte_strlcpy(name, mz->name, RTE_MEMZONE_NAMESIZE);
	mcfg = rte_eal_get_configuration()->mem_config;
	arr = &mcfg->memzones;

	rte_rwlock_write_lock(&mcfg->mlock);

	idx = rte_fbarray_find_idx(arr, mz);
	found_mz = rte_fbarray_get(arr, idx);

	if (found_mz == NULL) {
		ret = -EINVAL;
	} else if (found_mz->addr == NULL) {
		EAL_LOG(ERR, "Memzone is not allocated");
		ret = -EINVAL;
	} else {
		addr = found_mz->addr;
		memset(found_mz, 0, sizeof(*found_mz));
		rte_fbarray_set_free(arr, idx);
	}

	rte_rwlock_write_unlock(&mcfg->mlock);

	rte_eal_trace_memzone_free(name, addr, ret);

	rte_free(addr);

	return ret;
}

/*
 * Lookup for the memzone identified by the given name
 */
RTE_EXPORT_SYMBOL(rte_memzone_lookup)
const struct rte_memzone *
rte_memzone_lookup(const char *name)
{
	struct rte_mem_config *mcfg;
	const struct rte_memzone *memzone = NULL;

	mcfg = rte_eal_get_configuration()->mem_config;

	rte_rwlock_read_lock(&mcfg->mlock);

	memzone = memzone_lookup_thread_unsafe(name);

	rte_rwlock_read_unlock(&mcfg->mlock);

	rte_eal_trace_memzone_lookup(name, memzone);
	return memzone;
}

struct memzone_info {
	FILE *f;
	uint64_t total_size;
};

static void
dump_memzone(const struct rte_memzone *mz, void *arg)
{
	struct rte_mem_config *mcfg = rte_eal_get_configuration()->mem_config;
	struct rte_memseg_list *msl = NULL;
	struct memzone_info *info = arg;
	void *cur_addr, *mz_end;
	struct rte_memseg *ms;
	int mz_idx, ms_idx;
	FILE *f = info->f;
	size_t page_sz;

	mz_idx = rte_fbarray_find_idx(&mcfg->memzones, mz);
	info->total_size += mz->len;

	fprintf(f, "Zone %u: name:<%s>, len:0x%zx, virt:%p, "
				"socket_id:%"PRId32", flags:%"PRIx32"\n",
			mz_idx,
			mz->name,
			mz->len,
			mz->addr,
			mz->socket_id,
			mz->flags);

	/* go through each page occupied by this memzone */
	msl = rte_mem_virt2memseg_list(mz->addr);
	if (!msl) {
		EAL_LOG(DEBUG, "Skipping bad memzone");
		return;
	}
	page_sz = (size_t)mz->hugepage_sz;
	cur_addr = RTE_PTR_ALIGN_FLOOR(mz->addr, page_sz);
	mz_end = RTE_PTR_ADD(cur_addr, mz->len);

	fprintf(f, "physical segments used:\n");
	ms_idx = RTE_PTR_DIFF(mz->addr, msl->base_va) / page_sz;
	ms = rte_fbarray_get(&msl->memseg_arr, ms_idx);

	do {
		fprintf(f, "  addr: %p iova: 0x%" PRIx64 " "
				"len: 0x%zx "
				"pagesz: 0x%zx\n",
			cur_addr, ms->iova, ms->len, page_sz);

		/* advance VA to next page */
		cur_addr = RTE_PTR_ADD(cur_addr, page_sz);

		/* memzones occupy contiguous segments */
		++ms;
	} while (cur_addr < mz_end);
}

/* Dump all reserved memory zones on console */
RTE_EXPORT_SYMBOL(rte_memzone_dump)
void
rte_memzone_dump(FILE *f)
{
	struct memzone_info info = { .f = f };

	rte_memzone_walk(dump_memzone, &info);
	fprintf(f, "Total Memory Zones size = %"PRIu64"M\n",
		info.total_size / (1024 * 1024));
}

/*
 * Init the memzone subsystem
 */
int
rte_eal_memzone_init(void)
{
	struct rte_mem_config *mcfg;
	int ret = 0;

	/* get pointer to global configuration */
	mcfg = rte_eal_get_configuration()->mem_config;

	rte_rwlock_write_lock(&mcfg->mlock);

	if (rte_eal_process_type() == RTE_PROC_PRIMARY &&
			rte_fbarray_init(&mcfg->memzones, "memzone",
			rte_memzone_max_get(), sizeof(struct rte_memzone))) {
		EAL_LOG(ERR, "Cannot allocate memzone list");
		ret = -1;
	} else if (rte_eal_process_type() == RTE_PROC_SECONDARY &&
			rte_fbarray_attach(&mcfg->memzones)) {
		EAL_LOG(ERR, "Cannot attach to memzone list");
		ret = -1;
	}

	rte_rwlock_write_unlock(&mcfg->mlock);

	return ret;
}

/* Walk all reserved memory zones */
RTE_EXPORT_SYMBOL(rte_memzone_walk)
void rte_memzone_walk(void (*func)(const struct rte_memzone *, void *),
		      void *arg)
{
	struct rte_mem_config *mcfg;
	struct rte_fbarray *arr;
	int i;

	mcfg = rte_eal_get_configuration()->mem_config;
	arr = &mcfg->memzones;

	rte_rwlock_read_lock(&mcfg->mlock);
	i = rte_fbarray_find_next_used(arr, 0);
	while (i >= 0) {
		struct rte_memzone *mz = rte_fbarray_get(arr, i);
		(*func)(mz, arg);
		i = rte_fbarray_find_next_used(arr, i + 1);
	}
	rte_rwlock_read_unlock(&mcfg->mlock);
}
