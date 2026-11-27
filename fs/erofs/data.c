// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 */
#include "internal.h"
#include <linux/prefetch.h>

#include <trace/events/erofs.h>

static void erofs_readendio(struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	const blk_status_t err = bio->bi_status;

	bio_for_each_segment_all(bvec, bio, i) {
		struct page *page = bvec->bv_page;

		/* page is already locked */
		DBG_BUGON(PageUptodate(page));

		if (err)
			SetPageError(page);
		else
			SetPageUptodate(page);

		unlock_page(page);
		/* page could be reclaimed now */
	}
	bio_put(bio);
}

struct page *erofs_get_meta_page(struct super_block *sb, erofs_blk_t blkaddr)
{
	struct address_space *const mapping = sb->s_bdev->bd_inode->i_mapping;
	struct page *page;

	page = read_cache_page_gfp(mapping, blkaddr,
				   mapping_gfp_constraint(mapping, ~__GFP_FS));
	/* should already be PageUptodate */
	if (!IS_ERR(page))
		lock_page(page);
	return page;
}

void erofs_unmap_metabuf(struct erofs_buf *buf)
{
	if (buf->kmap_type == EROFS_KMAP)
		kunmap(buf->page);
	else if (buf->kmap_type == EROFS_KMAP_ATOMIC)
		kunmap_atomic(buf->base);
	buf->base = NULL;
	buf->kmap_type = EROFS_NO_KMAP;
}

void erofs_put_metabuf(struct erofs_buf *buf)
{
	if (!buf->page)
		return;
	erofs_unmap_metabuf(buf);
	put_page(buf->page);
	buf->page = NULL;
}

void *erofs_read_metabuf(struct erofs_buf *buf, struct super_block *sb,
			erofs_blk_t blkaddr, enum erofs_kmap_type type)
{
	struct address_space *const mapping = sb->s_bdev->bd_inode->i_mapping;
	erofs_off_t offset = blknr_to_addr(blkaddr);
	pgoff_t index = offset >> PAGE_SHIFT;
	struct page *page = buf->page;

	if (!page || page->index != index) {
		struct folio *folio;

		erofs_put_metabuf(buf);
		folio = read_cache_folio_gfp(mapping, index,
				mapping_gfp_constraint(mapping, ~__GFP_FS));
		if (IS_ERR(folio))
			return ERR_CAST(folio);
		/* should already be PageUptodate, no need to lock page */
		page = folio_file_page(folio, index);
		buf->page = page;
	}
	if (buf->kmap_type == EROFS_NO_KMAP) {
		if (type == EROFS_KMAP)
			buf->base = kmap(page);
		else if (type == EROFS_KMAP_ATOMIC)
			buf->base = kmap_atomic(page);
		buf->kmap_type = type;
	} else if (buf->kmap_type != type) {
		DBG_BUGON(1);
		return ERR_PTR(-EFAULT);
	}
	if (type == EROFS_NO_KMAP)
		return NULL;
	return buf->base + (offset & ~PAGE_MASK);
}

int erofs_map_dev(struct super_block *sb, struct erofs_map_dev *map)
{
	struct erofs_dev_context *devs = EROFS_SB(sb)->devs;
	struct erofs_device_info *dif;
	erofs_off_t startoff;
	int id;

	map->m_sb = sb;
	map->m_dif = &EROFS_SB(sb)->dif0;
	map->m_bdev = sb->s_bdev;

	if (map->m_deviceid) {
		down_read(&devs->rwsem);
		dif = idr_find(&devs->tree, map->m_deviceid - 1);
		if (!dif) {
			up_read(&devs->rwsem);
			return -ENODEV;
		}
		if (devs->flatdev) {
			map->m_pa += (erofs_off_t)dif->uniaddr << sb->s_blocksize_bits;
			up_read(&devs->rwsem);
			return 0;
		}
		map->m_dif = dif;
		map->m_bdev = dif->bdev;
		up_read(&devs->rwsem);
	} else if (devs && devs->extra_devices && !devs->flatdev) {
		down_read(&devs->rwsem);
		idr_for_each_entry(&devs->tree, dif, id) {
			if (!dif->uniaddr)
				continue;

			startoff = (erofs_off_t)dif->uniaddr << sb->s_blocksize_bits;
			if (map->m_pa >= startoff &&
			    map->m_pa < startoff + ((erofs_off_t)dif->blocks << sb->s_blocksize_bits)) {
				map->m_pa -= startoff;
				map->m_dif = dif;
				map->m_bdev = dif->bdev;
				break;
			}
		}
		up_read(&devs->rwsem);
	}
	return 0;
}

int erofs_map_chunks(struct inode *inode, struct erofs_map_blocks *map)
{
	struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
	struct super_block *sb = inode->i_sb;
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_inode_chunk_index *idx;
	unsigned int unit = (vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES) ?
		sizeof(*idx) : EROFS_BLOCK_MAP_ENTRY_SIZE;
	u64 nr = map->m_la >> vi->chunkbits, chunksize = 1ULL << vi->chunkbits;
	erofs_off_t pos = ALIGN(iloc(EROFS_SB(sb), vi->nid) + vi->inode_isize +
				vi->xattr_isize, unit) + unit * nr;
	erofs_off_t endpos = round_up(pos + 1, sb->s_blocksize);
	u64 addrmask, startblk, last;
	void *kaddr;

	kaddr = erofs_read_metabuf(&buf, sb, erofs_blknr(pos), EROFS_KMAP_ATOMIC);
	if (IS_ERR(kaddr))
		return PTR_ERR(kaddr);

	idx = kaddr + erofs_blkoff(pos);
	if (vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES) {
		addrmask = (vi->chunkformat & EROFS_CHUNK_FORMAT_48BIT) ?
			BIT_ULL(48) - 1 : BIT_ULL(32) - 1;
		startblk = (((u64)le16_to_cpu(idx->startblk_hi) << 32) |
			    le32_to_cpu(idx->startblk_lo)) & addrmask;
		last = startblk;
		if ((startblk ^ EROFS_NULL_ADDR) & addrmask) {
			map->m_deviceid = le16_to_cpu(idx->device_id) &
				EROFS_SB(sb)->device_id_mask;
			map->m_pa = erofs_pos(sb, startblk);
			map->m_flags = EROFS_MAP_MAPPED;
		} else {
			map->m_flags = 0;
		}
	} else {
		startblk = le32_to_cpu(*(__le32 *)idx);
		last = startblk;
		if (startblk != (u32)EROFS_NULL_ADDR) {
			map->m_pa = erofs_pos(sb, startblk);
			map->m_flags = EROFS_MAP_MAPPED;
		} else {
			map->m_flags = 0;
		}
	}
	map->m_la = nr << vi->chunkbits;
	map->m_llen = chunksize;

	while ((pos += unit) < endpos) {
		idx = (void *)idx + unit;
		if (vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES) {
			u64 cur = (((u64)le16_to_cpu(idx->startblk_hi) << 32) |
				   le32_to_cpu(idx->startblk_lo)) & addrmask;
			if (cur != last)
				break;
		} else {
			u32 cur = le32_to_cpu(*(__le32 *)idx);
			if (cur != (u32)last)
				break;
		}
		map->m_llen += chunksize;
	}
	map->m_plen = map->m_llen;
	erofs_put_metabuf(&buf);
	return 0;
}

static int erofs_map_blocks_flatmode(struct inode *inode,
				     struct erofs_map_blocks *map,
				     int flags)
{
	int err = 0;
	erofs_blk_t nblocks, lastblk;
	u64 offset = map->m_la;
	struct erofs_inode *vi = EROFS_I(inode);
	bool tailendpacking = (vi->datalayout == EROFS_INODE_FLAT_INLINE);

	trace_erofs_map_blocks_flatmode_enter(inode, map, flags);

	map->m_deviceid = 0;
	if (vi->datalayout == EROFS_INODE_CHUNK_BASED) {
		err = erofs_map_chunks(inode, map);
		goto out;
	}

	nblocks = DIV_ROUND_UP(inode->i_size, PAGE_SIZE);
	lastblk = nblocks - tailendpacking;

	if (unlikely(offset >= inode->i_size)) {
		/* leave out-of-bound access unmapped */
		map->m_flags = 0;
		map->m_plen = 0;
		goto out;
	}

	/* there is no hole in flatmode */
	map->m_flags = EROFS_MAP_MAPPED;

	if (likely(offset < blknr_to_addr(lastblk))) {
		map->m_pa = blknr_to_addr(vi->raw_blkaddr) + map->m_la;
		map->m_plen = blknr_to_addr(lastblk) - offset;
	} else if (tailendpacking) {
		/* 2 - inode inline B: inode, [xattrs], inline last blk... */
		struct erofs_sb_info *sbi = EROFS_SB(inode->i_sb);

		map->m_pa = iloc(sbi, vi->nid) + vi->inode_isize +
			vi->xattr_isize + erofs_blkoff(map->m_la);
		map->m_plen = inode->i_size - offset;

		/* inline data should be located in one meta block */
		if (erofs_blkoff(map->m_pa) + map->m_plen > PAGE_SIZE) {
			erofs_err(inode->i_sb,
				  "inline data cross block boundary @ nid %llu",
				  vi->nid);
			DBG_BUGON(1);
			err = -EFSCORRUPTED;
			goto err_out;
		}

		map->m_flags |= EROFS_MAP_META;
	} else {
		erofs_err(inode->i_sb,
			  "internal error @ nid: %llu (size %llu), m_la 0x%llx",
			  vi->nid, inode->i_size, map->m_la);
		DBG_BUGON(1);
		err = -EIO;
		goto err_out;
	}

out:
	map->m_llen = map->m_plen;

err_out:
	trace_erofs_map_blocks_flatmode_exit(inode, map, flags, 0);
	return err;
}

static inline struct bio *erofs_read_raw_page(struct bio *bio,
					      struct address_space *mapping,
					      struct page *page,
					      erofs_off_t *last_block,
					      unsigned int nblocks,
					      bool ra)
{
	struct inode *const inode = mapping->host;
	struct super_block *const sb = inode->i_sb;
	erofs_off_t current_block = (erofs_off_t)page->index;
	int err;

	DBG_BUGON(!nblocks);

	if (PageUptodate(page)) {
		err = 0;
		goto has_updated;
	}

	/* note that for readpage case, bio also equals to NULL */
	if (bio &&
	    /* not continuous */
	    *last_block + 1 != current_block) {
submit_bio_retry:
		submit_bio(bio);
		bio = NULL;
	}

	if (!bio) {
		struct erofs_map_blocks map = {
			.m_la = blknr_to_addr(current_block),
		};
		erofs_blk_t blknr;
		unsigned int blkoff;

		err = erofs_map_blocks_flatmode(inode, &map, EROFS_GET_BLOCKS_RAW);
		if (err)
			goto err_out;

		/* zero out the holed page */
		if (!(map.m_flags & EROFS_MAP_MAPPED)) {
			zero_user_segment(page, 0, PAGE_SIZE);
			SetPageUptodate(page);

			/* imply err = 0, see erofs_map_blocks */
			goto has_updated;
		}

		/* for RAW access mode, m_plen must be equal to m_llen */
		DBG_BUGON(map.m_plen != map.m_llen);

		blknr = erofs_blknr(map.m_pa);
		blkoff = erofs_blkoff(map.m_pa);

		/* deal with inline page */
		if (map.m_flags & EROFS_MAP_META) {
			void *vsrc, *vto;
			struct page *ipage;

			DBG_BUGON(map.m_plen > PAGE_SIZE);

			ipage = erofs_get_meta_page(inode->i_sb, blknr);

			if (IS_ERR(ipage)) {
				err = PTR_ERR(ipage);
				goto err_out;
			}

			vsrc = kmap_atomic(ipage);
			vto = kmap_atomic(page);
			memcpy(vto, vsrc + blkoff, map.m_plen);
			memset(vto + map.m_plen, 0, PAGE_SIZE - map.m_plen);
			kunmap_atomic(vto);
			kunmap_atomic(vsrc);
			unlock_page(ipage);
			put_page(ipage);

			flush_dcache_page(page);
			SetPageUptodate(page);

			/* imply err = 0, see erofs_map_blocks */
			goto has_updated;
		}

		/* pa must be block-aligned for raw reading */
		DBG_BUGON(erofs_blkoff(map.m_pa));

		/* max # of continuous pages */
		if (nblocks > DIV_ROUND_UP(map.m_plen, PAGE_SIZE))
			nblocks = DIV_ROUND_UP(map.m_plen, PAGE_SIZE);
		if (nblocks > BIO_MAX_PAGES)
			nblocks = BIO_MAX_PAGES;

		{
			struct erofs_map_dev mdev;

			mdev.m_deviceid = map.m_deviceid;
			mdev.m_pa = map.m_pa;
			err = erofs_map_dev(sb, &mdev);
			if (err)
				goto err_out;

			bio = bio_alloc(GFP_NOIO, nblocks);

			bio->bi_end_io = erofs_readendio;
			bio_set_dev(bio, mdev.m_bdev);
			bio->bi_iter.bi_sector = (sector_t)(mdev.m_pa >> 9);
			bio->bi_opf = REQ_OP_READ;
			if (ra)
				bio->bi_opf |= REQ_RAHEAD;
		}
	}

	err = bio_add_page(bio, page, PAGE_SIZE, 0);
	/* out of the extent or bio is full */
	if (err < PAGE_SIZE)
		goto submit_bio_retry;

	*last_block = current_block;

	/* shift in advance in case of it followed by too many gaps */
	if (bio->bi_iter.bi_size >= bio->bi_max_vecs * PAGE_SIZE) {
		/* err should reassign to 0 after submitting */
		err = 0;
		goto submit_bio_out;
	}

	return bio;

err_out:
	/* for sync reading, set page error immediately */
	if (!ra) {
		SetPageError(page);
		ClearPageUptodate(page);
	}
has_updated:
	unlock_page(page);

	/* if updated manually, continuous pages has a gap */
	if (bio)
submit_bio_out:
		submit_bio(bio);
	return err ? ERR_PTR(err) : NULL;
}

/*
 * since we dont have write or truncate flows, so no inode
 * locking needs to be held at the moment.
 */
static int erofs_raw_access_read_folio(struct file *file, struct folio *folio)
{
	struct page *page = folio_page(folio, 0);
	erofs_off_t last_block;
	struct bio *bio;

	trace_erofs_readpage(page, true);

	bio = erofs_read_raw_page(NULL, page->mapping,
				  page, &last_block, 1, false);

	if (IS_ERR(bio))
		return PTR_ERR(bio);

	DBG_BUGON(bio);	/* since we have only one bio -- must be NULL */
	return 0;
}

static int erofs_raw_access_readpage(struct file *file, struct page *page)
{
	return erofs_raw_access_read_folio(file, page_folio(page));
}

static int erofs_raw_access_readpages(struct file *filp,
				      struct address_space *mapping,
				      struct list_head *pages,
				      unsigned int nr_pages)
{
	erofs_off_t last_block;
	struct bio *bio = NULL;
	gfp_t gfp = readahead_gfp_mask(mapping);
	struct page *page = list_last_entry(pages, struct page, lru);

	trace_erofs_readpages(mapping->host, page, nr_pages, true);

	for (; nr_pages; --nr_pages) {
		page = list_entry(pages->prev, struct page, lru);

		prefetchw(&page->flags);
		list_del(&page->lru);

		if (!add_to_page_cache_lru(page, mapping, page->index, gfp)) {
			bio = erofs_read_raw_page(bio, mapping, page,
						  &last_block, nr_pages, true);

			/* all the page errors are ignored when readahead */
			if (IS_ERR(bio)) {
				pr_err("%s, readahead error at page %lu of nid %llu\n",
				       __func__, page->index,
				       EROFS_I(mapping->host)->nid);

				bio = NULL;
			}
		}

		/* pages could still be locked */
		put_page(page);
	}
	DBG_BUGON(!list_empty(pages));

	/* the rare case (end in gaps) */
	if (bio)
		submit_bio(bio);
	return 0;
}

static sector_t erofs_bmap(struct address_space *mapping, sector_t block)
{
	struct inode *inode = mapping->host;
	struct erofs_map_blocks map = {
		.m_la = blknr_to_addr(block),
	};

	if (EROFS_I(inode)->datalayout == EROFS_INODE_FLAT_INLINE) {
		erofs_blk_t blks = i_size_read(inode) >> LOG_BLOCK_SIZE;

		if (block >> LOG_SECTORS_PER_BLOCK >= blks)
			return 0;
	}

	if (!erofs_map_blocks_flatmode(inode, &map, EROFS_GET_BLOCKS_RAW))
		return erofs_blknr(map.m_pa);

	return 0;
}

/* for uncompressed (aligned) files and raw access for other files */
const struct address_space_operations erofs_raw_access_aops = {
	.readpage = erofs_raw_access_readpage,
	.readpages = erofs_raw_access_readpages,
	.bmap = erofs_bmap,
};
