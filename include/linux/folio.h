/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FOLIO_H
#define _LINUX_FOLIO_H

#include <linux/mm.h>

static inline struct folio *page_folio(struct page *page)
{
	return (struct folio *)compound_head(page);
}
#define page_folio page_folio

static inline struct page *folio_page(struct folio *folio, size_t n)
{
	return &folio->page + n;
}

static inline struct page *folio_file_page(struct folio *folio, pgoff_t index)
{
	return folio_page(folio, index - folio->page.index);
}

static inline size_t folio_size(struct folio *folio)
{
	return PAGE_SIZE << compound_order(&folio->page);
}

static inline unsigned int folio_order(struct folio *folio)
{
	return compound_order(&folio->page);
}

static inline pgoff_t folio_index(struct folio *folio)
{
	return folio->page.index;
}

static inline void *folio_address(struct folio *folio)
{
	return page_address(&folio->page);
}

static inline struct address_space *folio_mapping(struct folio *folio)
{
	return folio->page.mapping;
}

static inline void folio_get(struct folio *folio)
{
	get_page(&folio->page);
}

static inline void folio_put(struct folio *folio)
{
	put_page(&folio->page);
}

static inline void folio_lock(struct folio *folio)
{
	lock_page(&folio->page);
}

static inline void folio_unlock(struct folio *folio)
{
	unlock_page(&folio->page);
}

static inline bool folio_trylock(struct folio *folio)
{
	return trylock_page(&folio->page);
}

static inline void folio_mark_uptodate(struct folio *folio)
{
	SetPageUptodate(&folio->page);
}

static inline void folio_mark_error(struct folio *folio)
{
	SetPageError(&folio->page);
}

static inline bool folio_test_uptodate(struct folio *folio)
{
	return PageUptodate(&folio->page);
}

static inline bool folio_test_locked(struct folio *folio)
{
	return PageLocked(&folio->page);
}

static inline bool folio_test_error(struct folio *folio)
{
	return PageError(&folio->page);
}

static inline struct folio *read_cache_folio(struct address_space *mapping,
					     pgoff_t index,
					     int (*filler)(void *, struct page *),
					     void *data)
{
	struct page *page;

	if (filler)
		page = read_cache_page(mapping, index, filler, data);
	else
		page = read_mapping_page(mapping, index, data);

	if (IS_ERR(page))
		return ERR_CAST(page);

	return page_folio(page);
}

static inline struct folio *read_cache_folio_gfp(struct address_space *mapping,
						 pgoff_t index,
						 gfp_t gfp)
{
	struct page *page = read_cache_page_gfp(mapping, index, gfp);

	if (IS_ERR(page))
		return ERR_CAST(page);

	return page_folio(page);
}

#endif /* _LINUX_FOLIO_H */
