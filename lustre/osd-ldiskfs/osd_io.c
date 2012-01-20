/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_io.c
 *
 * body operations
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 * Author: Alex Zhuravlev <bzzz@sun.com>
 *
 */

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/* prerequisite for linux/xattr.h */
#include <linux/types.h>
/* prerequisite for linux/xattr.h */
#include <linux/fs.h>

/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <obd_support.h>
/* struct ptlrpc_thread */
#include <lustre_net.h>

/* fid_is_local() */
#include <lustre_fid.h>

#include "osd_internal.h"

int osd_object_auth(const struct lu_env *env, struct dt_object *dt,
                    struct lustre_capa *capa, __u64 opc);

static void osd_init_iobuf(struct osd_device *d, struct osd_iobuf *iobuf, int rw)
{

        cfs_waitq_init(&iobuf->dr_wait);
        cfs_atomic_set(&iobuf->dr_numreqs, 0);
        iobuf->dr_max_pages = PTLRPC_MAX_BRW_PAGES;
        iobuf->dr_npages = 0;
        iobuf->dr_error = 0;
        iobuf->dr_dev = d;
        iobuf->dr_frags = 0;
        iobuf->dr_elapsed = 0;
        /* must be counted before, so assert */
        LASSERT(iobuf->dr_elapsed_valid == 0);
        iobuf->dr_rw = rw;
}

static void osd_iobuf_add_page(struct osd_iobuf *iobuf, struct page *page)
{
        LASSERT(iobuf->dr_npages < iobuf->dr_max_pages);
        iobuf->dr_pages[iobuf->dr_npages++] = page;
}

void osd_fini_iobuf(struct osd_device *d, struct osd_iobuf *iobuf)
{
        int rw = iobuf->dr_rw;

        if (iobuf->dr_elapsed_valid) {
                iobuf->dr_elapsed_valid = 0;
                LASSERT(iobuf->dr_dev == d);
                LASSERT(iobuf->dr_frags > 0);
                lprocfs_oh_tally(&d->od_brw_stats.
                                 hist[BRW_R_DIO_FRAGS+rw],
                                 iobuf->dr_frags);
                lprocfs_oh_tally_log2(&d->od_brw_stats.hist[BRW_R_IO_TIME+rw],
                                      iobuf->dr_elapsed);
        }
}

#ifdef HAVE_BIO_ENDIO_2ARG
#define DIO_RETURN(a)
static void dio_complete_routine(struct bio *bio, int error)
#else
#define DIO_RETURN(a)   return(a)
static int dio_complete_routine(struct bio *bio, unsigned int done, int error)
#endif
{
        struct osd_iobuf *iobuf = bio->bi_private;
        struct bio_vec *bvl;
        int i;

        /* CAVEAT EMPTOR: possibly in IRQ context
         * DO NOT record procfs stats here!!! */

#ifdef HAVE_BIO_ENDIO_2ARG
       /* The "bi_size" check was needed for kernels < 2.6.24 in order to
        * handle the case where a SCSI request error caused this callback
        * to be called before all of the biovecs had been processed.
        * Without this check the server thread will hang.  In newer kernels
        * the bio_end_io routine is never called for partial completions,
        * so this check is no longer needed. */
        if (bio->bi_size)                      /* Not complete */
                DIO_RETURN(1);
#endif

        if (unlikely(iobuf == NULL)) {
                CERROR("***** bio->bi_private is NULL!  This should never "
                       "happen.  Normally, I would crash here, but instead I "
                       "will dump the bio contents to the console.  Please "
                       "report this to <http://bugzilla.lustre.org/> , along "
                       "with any interesting messages leading up to this point "
                       "(like SCSI errors, perhaps).  Because bi_private is "
                       "NULL, I can't wake up the thread that initiated this "
                       "IO - you will probably have to reboot this node.\n");
                CERROR("bi_next: %p, bi_flags: %lx, bi_rw: %lu, bi_vcnt: %d, "
                       "bi_idx: %d, bi->size: %d, bi_end_io: %p, bi_cnt: %d, "
                       "bi_private: %p\n", bio->bi_next, bio->bi_flags,
                       bio->bi_rw, bio->bi_vcnt, bio->bi_idx, bio->bi_size,
                       bio->bi_end_io, cfs_atomic_read(&bio->bi_cnt),
                       bio->bi_private);
                DIO_RETURN(0);
        }

        /* the check is outside of the cycle for performance reason -bzzz */
        if (!cfs_test_bit(BIO_RW, &bio->bi_rw)) {
                bio_for_each_segment(bvl, bio, i) {
                        if (likely(error == 0))
                                SetPageUptodate(bvl->bv_page);
                        LASSERT(PageLocked(bvl->bv_page));
                        ClearPageConstant(bvl->bv_page);
                }
                cfs_atomic_dec(&iobuf->dr_dev->od_r_in_flight);
        } else {
                struct page *p = iobuf->dr_pages[0];
                if (p->mapping) {
                        if (mapping_cap_page_constant_write(p->mapping)){
                                bio_for_each_segment(bvl, bio, i) {
                                        ClearPageConstant(bvl->bv_page);
                                }
                        }
                }
                cfs_atomic_dec(&iobuf->dr_dev->od_w_in_flight);
        }

        /* any real error is good enough -bzzz */
        if (error != 0 && iobuf->dr_error == 0)
                iobuf->dr_error = error;

        if (cfs_atomic_dec_and_test(&iobuf->dr_numreqs)) {
                iobuf->dr_elapsed = jiffies - iobuf->dr_start_time;
                iobuf->dr_elapsed_valid = 1;
                cfs_waitq_signal(&iobuf->dr_wait);
        }

        /* Completed bios used to be chained off iobuf->dr_bios and freed in
         * filter_clear_dreq().  It was then possible to exhaust the biovec-256
         * mempool when serious on-disk fragmentation was encountered,
         * deadlocking the OST.  The bios are now released as soon as complete
         * so the pool cannot be exhausted while IOs are competing. bug 10076 */
        bio_put(bio);
        DIO_RETURN(0);
}

static void record_start_io(struct osd_iobuf *iobuf, int size)
{
        struct osd_device    *osd = iobuf->dr_dev;
        struct obd_histogram *h = osd->od_brw_stats.hist;

        iobuf->dr_frags++;
        cfs_atomic_inc(&iobuf->dr_numreqs);

        if (iobuf->dr_rw == 0) {
                cfs_atomic_inc(&osd->od_r_in_flight);
                lprocfs_oh_tally(&h[BRW_R_RPC_HIST],
                                 cfs_atomic_read(&osd->od_r_in_flight));
                lprocfs_oh_tally_log2(&h[BRW_R_DISK_IOSIZE], size);
        } else if (iobuf->dr_rw == 1) {
                cfs_atomic_inc(&osd->od_w_in_flight);
                lprocfs_oh_tally(&h[BRW_W_RPC_HIST],
                                 cfs_atomic_read(&osd->od_w_in_flight));
                lprocfs_oh_tally_log2(&h[BRW_W_DISK_IOSIZE], size);
        } else {
                LBUG();
        }
}

static void osd_submit_bio(int rw, struct bio *bio)
{
        LASSERTF(rw == 0 || rw == 1, "%x\n", rw);
        if (rw == 0)
                submit_bio(READ, bio);
        else
                submit_bio(WRITE, bio);
}

static int can_be_merged(struct bio *bio, sector_t sector)
{
        unsigned int size;

        if (!bio)
                return 0;

        size = bio->bi_size >> 9;
        return bio->bi_sector + size == sector ? 1 : 0;
}

static int osd_do_bio(struct osd_device *osd, struct inode *inode,
                      struct osd_iobuf *iobuf)
{
        int            blocks_per_page = CFS_PAGE_SIZE >> inode->i_blkbits;
        struct page  **pages = iobuf->dr_pages;
        int            npages = iobuf->dr_npages;
        unsigned long *blocks = iobuf->dr_blocks;
        int            total_blocks = npages * blocks_per_page;
        int            sector_bits = inode->i_sb->s_blocksize_bits - 9;
        unsigned int   blocksize = inode->i_sb->s_blocksize;
        struct bio    *bio = NULL;
        struct page   *page;
        unsigned int   page_offset;
        sector_t       sector;
        int            nblocks;
        int            block_idx;
        int            page_idx;
        int            i;
        int            rc = 0;
        ENTRY;

        LASSERT(iobuf->dr_npages == npages);

        osd_brw_stats_update(osd, iobuf);
        iobuf->dr_start_time = cfs_time_current();

        for (page_idx = 0, block_idx = 0;
             page_idx < npages;
             page_idx++, block_idx += blocks_per_page) {

                page = pages[page_idx];
                LASSERT (block_idx + blocks_per_page <= total_blocks);

                for (i = 0, page_offset = 0;
                     i < blocks_per_page;
                     i += nblocks, page_offset += blocksize * nblocks) {

                        nblocks = 1;

                        if (blocks[block_idx + i] == 0) {  /* hole */
                                LASSERTF(iobuf->dr_rw == 0,
                                         "page_idx %u, block_idx %u, i %u\n",
                                         page_idx, block_idx, i);
                                memset(kmap(page) + page_offset, 0, blocksize);
                                kunmap(page);
                                continue;
                        }

                        sector = (sector_t)blocks[block_idx + i] << sector_bits;

                        /* Additional contiguous file blocks? */
                        while (i + nblocks < blocks_per_page &&
                               (sector + (nblocks << sector_bits)) ==
                               ((sector_t)blocks[block_idx + i + nblocks] <<
                                sector_bits))
                                nblocks++;

                        /* I only set the page to be constant only if it
                         * is mapped to a contiguous underlying disk block(s).
                         * It will then make sure the corresponding device
                         * cache of raid5 will be overwritten by this page.
                         * - jay */
                        if (iobuf->dr_rw && (nblocks == blocks_per_page) &&
                            mapping_cap_page_constant_write(inode->i_mapping))
                               SetPageConstant(page);

                        if (bio != NULL &&
                            can_be_merged(bio, sector) &&
                            bio_add_page(bio, page,
                                         blocksize * nblocks, page_offset) != 0)
                                continue;       /* added this frag OK */

                        if (bio != NULL) {
                                struct request_queue *q =
                                        bdev_get_queue(bio->bi_bdev);

                                /* Dang! I have to fragment this I/O */
                                CDEBUG(D_INODE, "bio++ sz %d vcnt %d(%d) "
                                       "sectors %d(%d) psg %d(%d) hsg %d(%d)\n",
                                       bio->bi_size,
                                       bio->bi_vcnt, bio->bi_max_vecs,
                                       bio->bi_size >> 9, queue_max_sectors(q),
                                       bio_phys_segments(q, bio),
                                       queue_max_phys_segments(q),
                                       bio_hw_segments(q, bio),
                                       queue_max_hw_segments(q));

                                record_start_io(iobuf, bio->bi_size);
                                osd_submit_bio(iobuf->dr_rw, bio);
                        }

                        /* allocate new bio, limited by max BIO size, b=9945 */
                        bio = bio_alloc(GFP_NOIO, max(BIO_MAX_PAGES,
                                                      (npages - page_idx) *
                                                      blocks_per_page));
                        if (bio == NULL) {
                                CERROR("Can't allocate bio %u*%u = %u pages\n",
                                       (npages - page_idx), blocks_per_page,
                                       (npages - page_idx) * blocks_per_page);
                                rc = -ENOMEM;
                                goto out;
                        }

                        bio->bi_bdev = inode->i_sb->s_bdev;
                        bio->bi_sector = sector;
                        bio->bi_end_io = dio_complete_routine;
                        bio->bi_private = iobuf;

                        rc = bio_add_page(bio, page,
                                          blocksize * nblocks, page_offset);
                        LASSERT (rc != 0);
                }
        }

        if (bio != NULL) {
                record_start_io(iobuf, bio->bi_size);
                osd_submit_bio(iobuf->dr_rw, bio);
                rc = 0;
        }

 out:
        /* in order to achieve better IO throughput, we don't wait for writes
         * completion here. instead we proceed with transaction commit in
         * parallel and wait for IO completion once transaction is stopped
         * see osd_trans_stop() for more details -bzzz */
        if (iobuf->dr_rw == 0) {
                cfs_wait_event(iobuf->dr_wait,
                               cfs_atomic_read(&iobuf->dr_numreqs) == 0);
        }

        if (rc == 0)
                rc = iobuf->dr_error;
        RETURN(rc);
}

static int osd_map_remote_to_local(loff_t offset, ssize_t len, int *nrpages,
                                   struct niobuf_local *lnb)
{
        ENTRY;

        *nrpages = 0;

        while (len > 0) {
                int poff = offset & (CFS_PAGE_SIZE - 1);
                int plen = CFS_PAGE_SIZE - poff;

                if (plen > len)
                        plen = len;
                lnb->lnb_file_offset = offset;
                lnb->lnb_page_offset = poff;
                lnb->lnb_len = plen;
                //lb->flags = rnb->flags;
                lnb->lnb_flags = 0;
                lnb->lnb_page = NULL;
                lnb->lnb_rc = 0;

                LASSERTF(plen <= len, "plen %u, len %lld\n", plen,
                         (long long) len);
                offset += plen;
                len -= plen;
                lnb++;
                (*nrpages)++;
        }

        RETURN(0);
}

struct page *osd_get_page(struct dt_object *dt, loff_t offset, int rw)
{
        struct inode      *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_device *d = osd_obj2dev(osd_dt_obj(dt));
        struct page       *page;

        LASSERT(inode);

        page = find_or_create_page(inode->i_mapping, offset >> CFS_PAGE_SHIFT,
                                   GFP_NOFS | __GFP_HIGHMEM);
        if (unlikely(page == NULL))
                lprocfs_counter_add(d->od_stats, LPROC_OSD_NO_PAGE, 1);

        return page;
}

/*
 * there are following "locks":
 * journal_start
 * i_alloc_sem
 * i_mutex
 * page lock

 * osd write path
    * lock page(s)
    * journal_start
    * truncate_sem

 * ext3 vmtruncate:
    * lock page (all pages), unlock
    * lock partial page
    * journal_start
    * truncate_mutex

 * ext4 vmtruncate:
    * lock pages, unlock
    * journal_start
    * lock partial page
    * i_data_sem

*/
int osd_get_bufs(const struct lu_env *env, struct dt_object *d, loff_t pos,
                 ssize_t len, struct niobuf_local *lnb, int rw,
                 struct lustre_capa *capa)
{
        struct osd_object   *obj    = osd_dt_obj(d);
        int npages, i, rc = 0;

        LASSERT(obj->oo_inode);

        osd_map_remote_to_local(pos, len, &npages, lnb);

        for (i = 0; i < npages; i++, lnb++) {

                /* We still set up for ungranted pages so that granted pages
                 * can be written to disk as they were promised, and portals
                 * needs to keep the pages all aligned properly. */
                lnb->lnb_obj = obj;

                lnb->lnb_page = osd_get_page(d, lnb->lnb_file_offset, rw);
                if (lnb->lnb_page == NULL)
                        GOTO(cleanup, rc = -ENOMEM);

                /* DLM locking protects us from write and truncate competing
                 * for same region, but truncate can leave dirty page in the
                 * cache. it's possible the writeout on a such a page is in
                 * progress when we access it. it's also possible that during
                 * this writeout we put new (partial) data, but then won't
                 * be able to proceed in filter_commitrw_write(). thus let's
                 * just wait for writeout completion, should be rare enough.
                 * -bzzz */
                wait_on_page_writeback(lnb->lnb_page);
                BUG_ON(PageWriteback(lnb->lnb_page));

                lu_object_get(&d->do_lu);
        }
        rc = i;

cleanup:
        RETURN(rc);
}

static int osd_put_bufs(const struct lu_env *env, struct dt_object *dt,
                        struct niobuf_local *lnb, int npages)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf       *iobuf = &oti->oti_iobuf;
        struct osd_device      *d = osd_obj2dev(osd_dt_obj(dt));
        int                     i;

        /* to do IO stats, notice we do this here because
         * osd_do_bio() doesn't wait for write to complete */
        osd_fini_iobuf(d, iobuf);

        for (i = 0; i < npages; i++) {
                if (lnb[i].lnb_page == NULL)
                        continue;
                LASSERT(PageLocked(lnb[i].lnb_page));
                unlock_page(lnb[i].lnb_page);
                page_cache_release(lnb[i].lnb_page);
                lu_object_put(env, &dt->do_lu);
                lnb[i].lnb_page = NULL;
        }
        RETURN(0);
}

static int osd_write_prep(const struct lu_env *env, struct dt_object *dt,
                          struct niobuf_local *lnb, int npages)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf *iobuf = &oti->oti_iobuf;
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_device  *osd = osd_obj2dev(osd_dt_obj(dt));
        struct timeval start, end;
        unsigned long timediff;
        ssize_t isize;
        __s64 maxidx;
        int rc = 0, i, cache = 0;

        LASSERT(inode);

        osd_init_iobuf(osd, iobuf, 0);

        isize = i_size_read(inode);
        maxidx = ((isize + CFS_PAGE_SIZE - 1) >> CFS_PAGE_SHIFT) - 1;

        if (osd->od_writethrough_cache)
                cache = 1;
        if (isize > osd->od_readcache_max_filesize)
                cache = 0;

        cfs_gettimeofday(&start);
        for (i = 0; i < npages; i++) {

                if (cache == 0)
                        generic_error_remove_page(inode->i_mapping,
                                                  lnb[i].lnb_page);

                /*
                 * till commit the content of the page is undefined
                 * we'll set it uptodate once bulk is done. otherwise
                 * subsequent reads can access non-stable data
                 */
                ClearPageUptodate(lnb[i].lnb_page);

                if (lnb[i].lnb_len == CFS_PAGE_SIZE)
                        continue;

                if (maxidx >= lnb[i].lnb_page->index) {
                        osd_iobuf_add_page(iobuf, lnb[i].lnb_page);
                } else {
                        long off;
                        char *p = kmap(lnb[i].lnb_page);

                        off = lnb[i].lnb_page_offset;
                        if (off)
                                memset(p, 0, off);
                        off = lnb[i].lnb_page_offset + lnb[i].lnb_len;
                        off &= ~CFS_PAGE_MASK;
                        if (off)
                                memset(p + off, 0, CFS_PAGE_SIZE - off);
                        kunmap(lnb[i].lnb_page);
                }
        }
        cfs_gettimeofday(&end);
        timediff = cfs_timeval_sub(&end, &start, NULL);
        lprocfs_counter_add(osd->od_stats, LPROC_OSD_GET_PAGE, timediff);

        if (iobuf->dr_npages) {
                rc = osd->od_fsops->fs_map_inode_pages(inode, iobuf->dr_pages,
                                                       iobuf->dr_npages,
                                                       iobuf->dr_blocks,
                                                       NULL, 0, NULL);
                if (likely(rc == 0)) {
                        rc = osd_do_bio(osd, inode, iobuf);
                        /* do IO stats for preparation reads */
                        osd_fini_iobuf(osd, iobuf);
                }
        }
        RETURN(rc);
}

static int osd_declare_write_commit(const struct lu_env *env,
                                    struct dt_object *dt,
                                    struct niobuf_local *lnb, int npages,
                                    struct thandle *handle)
{
        const struct osd_device *osd = osd_obj2dev(osd_dt_obj(dt));
        struct inode            *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_thandle      *oh;
        int                      extents = 1, depth, i, newblocks;
        int                      old;

        LASSERT(handle != NULL);
        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        old = oh->ot_credits;
        newblocks = npages;

        /* calculate number of extents (probably better to pass nb) */
        for (i = 1; i < npages; i++)
                if (lnb[i].lnb_file_offset !=
                    lnb[i-1].lnb_file_offset + lnb[i-1].lnb_len)
                        extents++;

        /*
         * each extent can go into new leaf causing a split
         * 5 is max tree depth: inode + 4 index blocks
         * with blockmaps, depth is 3 at most
         */
        if (LDISKFS_I(inode)->i_flags & LDISKFS_EXTENTS_FL) {
                /*
                 * many concurrent threads may grow tree by the time
                 * our transaction starts. so, consider 2 is a min depth
                 */
                depth = ext_depth(inode);
                depth = max(depth, 1) + 1;
                newblocks += depth;
                oh->ot_credits++; /* inode */
                oh->ot_credits += depth * 2 * extents;
        } else {
                depth = 3;
                newblocks += depth;
                oh->ot_credits++; /* inode */
                oh->ot_credits += depth * extents;
        }

        /* each new block can go in different group (bitmap + gd) */

        /* we can't dirt more bitmap blocks than exist */
        if (newblocks > LDISKFS_SB(osd_sb(osd))->s_groups_count)
                oh->ot_credits += LDISKFS_SB(osd_sb(osd))->s_groups_count;
        else
                oh->ot_credits += newblocks;

        /* we can't dirt more gd blocks than exist */
        if (newblocks > LDISKFS_SB(osd_sb(osd))->s_gdb_count)
                oh->ot_credits += LDISKFS_SB(osd_sb(osd))->s_gdb_count;
        else
                oh->ot_credits += newblocks;

        RETURN(0);
}

/* Check if a block is allocated or not */
static int osd_is_mapped(struct inode *inode, obd_size offset)
{
        sector_t (*fs_bmap)(struct address_space *, sector_t);

        fs_bmap = inode->i_mapping->a_ops->bmap;

        /* We can't know if we are overwriting or not */
        if (fs_bmap == NULL)
                return 0;

        if (fs_bmap(inode->i_mapping, offset >> inode->i_blkbits) == 0)
                return 0;

        return 1;
}

static int osd_write_commit(const struct lu_env *env, struct dt_object *dt,
                struct niobuf_local *lnb, int npages, struct thandle *thandle)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf *iobuf = &oti->oti_iobuf;
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_device  *osd = osd_obj2dev(osd_dt_obj(dt));
        loff_t isize;
        int rc = 0, i;

        LASSERT(inode);

        osd_init_iobuf(osd, iobuf, 1);
        isize = i_size_read(inode);

        for (i = 0; i < npages; i++) {
                if (lnb[i].lnb_rc == -ENOSPC &&
                    osd_is_mapped(inode, lnb[i].lnb_file_offset)) {
                        /* Allow the write to proceed if overwriting an
                         * existing block */
                        lnb[i].lnb_rc = 0;
                }

                if (lnb[i].lnb_rc) { /* ENOSPC, network RPC error, etc. */
                        CDEBUG(D_INODE, "Skipping [%d] == %d\n", i,
                               lnb[i].lnb_rc);
                        LASSERT(lnb[i].lnb_page);
                        generic_error_remove_page(inode->i_mapping,
                                                  lnb[i].lnb_page);
                        continue;
                }

                LASSERT(PageLocked(lnb[i].lnb_page));
                LASSERT(!PageWriteback(lnb[i].lnb_page));

                if (lnb[i].lnb_file_offset + lnb[i].lnb_len > isize)
                        isize = lnb[i].lnb_file_offset + lnb[i].lnb_len;

                /*
                 * Since write and truncate are serialized by oo_sem, even
                 * partial-page truncate should not leave dirty pages in the
                 * page cache.
                 */
                LASSERT(!PageDirty(lnb[i].lnb_page));

                SetPageUptodate(lnb[i].lnb_page);

                osd_iobuf_add_page(iobuf, lnb[i].lnb_page);
        }

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_MAPBLK_ENOSPC)) {
                rc = -ENOSPC;
        } else if (iobuf->dr_npages > 0) {
                rc = osd->od_fsops->fs_map_inode_pages(inode, iobuf->dr_pages,
                                                       iobuf->dr_npages,
                                                       iobuf->dr_blocks, NULL,
                                                       1, NULL);
        } else {
                /* no pages to write, no transno is needed */
                thandle->th_local = 1;
        }

        if (likely(rc == 0)) {
                if (isize > i_size_read(inode)) {
                        i_size_write(inode, isize);
                        LDISKFS_I(inode)->i_disksize = isize;
                        mark_inode_dirty(inode);
                }

                rc = osd_do_bio(osd, inode, iobuf);
                /* we don't do stats here as in read path because
                 * write is async: we'll do this in osd_put_bufs() */
        }

        if (unlikely(rc != 0)) {
                /* if write fails, we should drop pages from the cache */
                for (i = 0; i < npages; i++) {
                        if (lnb[i].lnb_page == NULL)
                                continue;
                        LASSERT(PageLocked(lnb[i].lnb_page));
                        generic_error_remove_page(inode->i_mapping,
                                                  lnb[i].lnb_page);
                }
        }

        RETURN(rc);
}

static int osd_read_prep(const struct lu_env *env, struct dt_object *dt,
                         struct niobuf_local *lnb, int npages)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf *iobuf = &oti->oti_iobuf;
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_device  *osd = osd_obj2dev(osd_dt_obj(dt));
        struct timeval start, end;
        unsigned long timediff;
        int rc = 0, i, m = 0, cache = 0;

        LASSERT(inode);

        osd_init_iobuf(osd, iobuf, 0);

        if (osd->od_read_cache)
                cache = 1;
        if (i_size_read(inode) > osd->od_readcache_max_filesize)
                cache = 0;

        cfs_gettimeofday(&start);
        for (i = 0; i < npages; i++) {

                if (i_size_read(inode) <= lnb[i].lnb_file_offset)
                        /* If there's no more data, abort early.
                         * lnb->lnb_rc == 0, so it's easy to detect later. */
                        break;

                if (i_size_read(inode) <
                    lnb[i].lnb_file_offset + lnb[i].lnb_len - 1)
                        lnb[i].lnb_rc = i_size_read(inode) -
                                        lnb[i].lnb_file_offset;
                else
                        lnb[i].lnb_rc = lnb[i].lnb_len;
                m += lnb[i].lnb_len;

                lprocfs_counter_add(osd->od_stats, LPROC_OSD_CACHE_ACCESS, 1);
                if (PageUptodate(lnb[i].lnb_page)) {
                        lprocfs_counter_add(osd->od_stats,
                                            LPROC_OSD_CACHE_HIT, 1);
                } else {
                        lprocfs_counter_add(osd->od_stats,
                                            LPROC_OSD_CACHE_MISS, 1);
                        osd_iobuf_add_page(iobuf, lnb[i].lnb_page);
                }
                if (cache == 0)
                        generic_error_remove_page(inode->i_mapping,
                                                  lnb[i].lnb_page);
        }
        cfs_gettimeofday(&end);
        timediff = cfs_timeval_sub(&end, &start, NULL);
        lprocfs_counter_add(osd->od_stats, LPROC_OSD_GET_PAGE, timediff);

        if (iobuf->dr_npages) {
                rc = osd->od_fsops->fs_map_inode_pages(inode, iobuf->dr_pages,
                                                       iobuf->dr_npages,
                                                       iobuf->dr_blocks,
                                                       NULL, 0, NULL);
                rc = osd_do_bio(osd, inode, iobuf);

                /* IO stats will be done in osd_bufs_put() */
        }

        RETURN(rc);
}

/*
 * XXX: Another layering violation for now.
 *
 * We don't want to use ->f_op->read methods, because generic file write
 *
 *         - serializes on ->i_sem, and
 *
 *         - does a lot of extra work like balance_dirty_pages(),
 *
 * which doesn't work for globally shared files like /last-received.
 */
static int osd_ldiskfs_readlink(struct inode *inode, char *buffer, int buflen)
{
        struct ldiskfs_inode_info *ei = LDISKFS_I(inode);

        memcpy(buffer, (char*)ei->i_data, buflen);

        return  buflen;
}

static int osd_ldiskfs_read(struct inode *inode, void *buf, int size,
                            loff_t *offs)
{
        struct buffer_head *bh;
        unsigned long block;
        int osize;
        int blocksize;
        int csize;
        int boffs;
        int err;

        /* prevent reading after eof */
        cfs_spin_lock(&inode->i_lock);
        if (i_size_read(inode) < *offs + size) {
                size = i_size_read(inode) - *offs;
                cfs_spin_unlock(&inode->i_lock);
                if (size < 0) {
                        CDEBUG(D_EXT2, "size %llu is too short to read @%llu\n",
                               i_size_read(inode), *offs);
                        return -EBADR;
                } else if (size == 0) {
                        return 0;
                }
        } else {
                cfs_spin_unlock(&inode->i_lock);
        }

        blocksize = 1 << inode->i_blkbits;
        osize = size;
        while (size > 0) {
                block = *offs >> inode->i_blkbits;
                boffs = *offs & (blocksize - 1);
                csize = min(blocksize - boffs, size);
                bh = ldiskfs_bread(NULL, inode, block, 0, &err);
                if (!bh) {
                        CERROR("can't read block: %d\n", err);
                        return err;
                }

                memcpy(buf, bh->b_data + boffs, csize);
                brelse(bh);

                *offs += csize;
                buf += csize;
                size -= csize;
        }
        return osize;
}


static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
                        struct lu_buf *buf, loff_t *pos,
                        struct lustre_capa *capa)
{
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        int           rc;

        if (osd_object_auth(env, dt, capa, CAPA_OPC_BODY_READ))
                RETURN(-EACCES);

        /* Read small symlink from inode body as we need to maintain correct
         * on-disk symlinks for ldiskfs.
         */
        if (S_ISLNK(dt->do_lu.lo_header->loh_attr) &&
            (buf->lb_len <= sizeof (LDISKFS_I(inode)->i_data)))
                rc = osd_ldiskfs_readlink(inode, buf->lb_buf, buf->lb_len);
        else
                rc = osd_ldiskfs_read(inode, buf->lb_buf, buf->lb_len, pos);

        return rc;
}

static ssize_t osd_declare_write(const struct lu_env *env, struct dt_object *dt,
                                 const loff_t size, loff_t pos,
                                 struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, write);
        oh->ot_credits += osd_dto_credits_noquota[DTO_WRITE_BLOCK];

        return 0;
}

static int osd_ldiskfs_writelink(struct inode *inode, char *buffer, int buflen)
{

        memcpy((char*)&LDISKFS_I(inode)->i_data, (char *)buffer,
               buflen);
        LDISKFS_I(inode)->i_disksize = buflen;
        i_size_write(inode, buflen);
        inode->i_sb->s_op->dirty_inode(inode);

        return 0;
}

static int osd_ldiskfs_write_record(struct inode *inode, void *buf, int bufsize,
                                    loff_t *offs, handle_t *handle)
{
        struct buffer_head *bh = NULL;
        loff_t offset = *offs;
        loff_t new_size = i_size_read(inode);
        unsigned long block;
        int blocksize = 1 << inode->i_blkbits;
        int err = 0;
        int size;
        int boffs;
        int dirty_inode = 0;

        while (bufsize > 0) {
                if (bh != NULL)
                        brelse(bh);

                block = offset >> inode->i_blkbits;
                boffs = offset & (blocksize - 1);
                size = min(blocksize - boffs, bufsize);
                bh = ldiskfs_bread(handle, inode, block, 1, &err);
                if (!bh) {
                        CERROR("can't read/create block: %d\n", err);
                        break;
                }

                err = ldiskfs_journal_get_write_access(handle, bh);
                if (err) {
                        CERROR("journal_get_write_access() returned error %d\n",
                               err);
                        break;
                }
                LASSERTF(boffs + size <= bh->b_size,
                         "boffs %d size %d bh->b_size %lu",
                         boffs, size, (unsigned long)bh->b_size);
                memcpy(bh->b_data + boffs, buf, size);
                err = ldiskfs_journal_dirty_metadata(handle, bh);
                if (err)
                        break;

                if (offset + size > new_size)
                        new_size = offset + size;
                offset += size;
                bufsize -= size;
                buf += size;
        }
        if (bh)
                brelse(bh);

        /* correct in-core and on-disk sizes */
        if (new_size > i_size_read(inode)) {
                cfs_spin_lock(&inode->i_lock);
                if (new_size > i_size_read(inode))
                        i_size_write(inode, new_size);
                if (i_size_read(inode) > LDISKFS_I(inode)->i_disksize) {
                        LDISKFS_I(inode)->i_disksize = i_size_read(inode);
                        dirty_inode = 1;
                }
                cfs_spin_unlock(&inode->i_lock);
                if (dirty_inode)
                        inode->i_sb->s_op->dirty_inode(inode);
        }

        if (err == 0)
                *offs = offset;
        return err;
}

static ssize_t osd_write(const struct lu_env *env, struct dt_object *dt,
                         const struct lu_buf *buf, loff_t *pos,
                         struct thandle *handle, struct lustre_capa *capa,
                         int ignore_quota)
{
        struct inode       *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_thandle *oh;
        ssize_t             result;
#ifdef HAVE_QUOTA_SUPPORT
        cfs_cap_t           save = current->cap_effective;
#endif

        LASSERT(dt_object_exists(dt));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_BODY_WRITE))
                return -EACCES;

        LASSERT(handle != NULL);

        /* XXX: don't check: one declared chunk can be used many times */
        /* OSD_EXEC_OP(handle, write); */

        oh = container_of(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle->h_transaction != NULL);
#ifdef HAVE_QUOTA_SUPPORT
        if (ignore_quota)
                current->cap_effective |= CFS_CAP_SYS_RESOURCE_MASK;
        else
                current->cap_effective &= ~CFS_CAP_SYS_RESOURCE_MASK;
#endif
        /* Write small symlink to inode body as we need to maintain correct
         * on-disk symlinks for ldiskfs.
         */
        if(S_ISLNK(dt->do_lu.lo_header->loh_attr) &&
           (buf->lb_len < sizeof (LDISKFS_I(inode)->i_data)))
                result = osd_ldiskfs_writelink(inode, buf->lb_buf, buf->lb_len);
        else
                result = osd_ldiskfs_write_record(inode, buf->lb_buf,
                                                  buf->lb_len, pos,
                                                  oh->ot_handle);
#ifdef HAVE_QUOTA_SUPPORT
        current->cap_effective = save;
#endif
        if (result == 0)
                result = buf->lb_len;
        return result;
}

static int osd_fiemap_get(const struct lu_env *env, struct dt_object *dt,
                          struct ll_user_fiemap *fm)
{
        struct inode *inode = osd_dt_obj(dt)->oo_inode;
        struct osd_thread_info *info   = osd_oti_get(env);
        struct dentry          *dentry = &info->oti_obj_dentry;
        struct file            *file   = &info->oti_file;
        mm_segment_t            saved_fs;
        int rc;

        LASSERT(inode);
        dentry->d_inode = inode;
        file->f_dentry = dentry;
        file->f_mapping = inode->i_mapping;
        file->f_op = inode->i_fop;

        saved_fs = get_fs();
        set_fs(get_ds());
#ifdef HAVE_EXT4_LDISKFS
        /* ldiskfs_ioctl does not have a inode argument */
        if (inode->i_fop->unlocked_ioctl)
                rc = inode->i_fop->unlocked_ioctl(file, FSFILT_IOC_FIEMAP,
                                                  (long)fm);
#else
        if (inode->i_fop->ioctl)
                rc = inode->i_fop->ioctl(inode, file, FSFILT_IOC_FIEMAP,
                                         (long)fm);
#endif
        else
                rc = -ENOTTY;
        set_fs(saved_fs);
        return rc;
}

/*
 * in some cases we may need declare methods for objects being created
 * e.g., when we create symlink
 */
const struct dt_body_operations osd_body_ops_new = {
        .dbo_declare_write = osd_declare_write,
};

const struct dt_body_operations osd_body_ops = {
        .dbo_read                 = osd_read,
        .dbo_declare_write        = osd_declare_write,
        .dbo_write                = osd_write,
        .dbo_get_bufs             = osd_get_bufs,
        .dbo_put_bufs             = osd_put_bufs,
        .dbo_write_prep           = osd_write_prep,
        .dbo_declare_write_commit = osd_declare_write_commit,
        .dbo_write_commit         = osd_write_commit,
        .dbo_read_prep            = osd_read_prep,
        .dbo_fiemap_get           = osd_fiemap_get,
};
