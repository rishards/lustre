/* -*- MODE: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  mdd/mdd_handler.c
 *  Lustre Metadata Server (mdd) routines
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *   Author: Wang Di <wangdi@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>

#include <linux/obd.h>
#include <linux/obd_class.h>
#include <linux/lustre_ver.h>
#include <linux/obd_support.h>
#include <linux/lprocfs_status.h>


#include <linux/lu_object.h>
#include <linux/md_object.h>
#include <linux/dt_object.h>

#include "mdd_internal.h"


static struct thandle* mdd_trans_start(struct lu_context *ctxt,
                                       struct mdd_device *,
                                       struct txn_param *);
static void mdd_trans_stop(struct lu_context *ctxt,
                           struct mdd_device *mdd, struct thandle *handle);
static struct dt_object* mdd_object_child(struct mdd_object *o);
static struct lu_device_operations mdd_lu_ops;
static void mdd_lock(struct lu_context *ctx,
                     struct mdd_object *obj, enum dt_lock_mode mode);
static void mdd_unlock(struct lu_context *ctx,
                       struct mdd_object *obj, enum dt_lock_mode mode);

static struct md_object_operations mdd_obj_ops;
static struct md_dir_operations    mdd_dir_ops;
static struct lu_object_operations mdd_lu_obj_ops;

static int lu_device_is_mdd(struct lu_device *d)
{
	/*
	 * XXX for now. Tags in lu_device_type->ldt_something are needed.
	 */
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &mdd_lu_ops);
}

static struct mdd_device* lu2mdd_dev(struct lu_device *d)
{
	LASSERT(lu_device_is_mdd(d));
	return container_of0(d, struct mdd_device, mdd_md_dev.md_lu_dev);
}

static inline struct lu_device *mdd2lu_dev(struct mdd_device *d)
{
	return (&d->mdd_md_dev.md_lu_dev);
}

static struct mdd_object *mdd_obj(struct lu_object *o)
{
	LASSERT(lu_device_is_mdd(o->lo_dev));
	return container_of0(o, struct mdd_object, mod_obj.mo_lu);
}

static struct mdd_device* mdo2mdd(struct md_object *mdo)
{
        return lu2mdd_dev(mdo->mo_lu.lo_dev);
}

static struct mdd_object* mdo2mddo(struct md_object *mdo)
{
        return container_of0(mdo, struct mdd_object, mod_obj);
}

static inline struct dt_device_operations *mdd_child_ops(struct mdd_device *d)
{
        return d->mdd_child->dd_ops;
}

static struct lu_object *mdd_object_alloc(struct lu_context *ctxt,
                                          struct lu_device *d)
{
        struct mdd_object *mdo;
        ENTRY;

        OBD_ALLOC_PTR(mdo);
        if (mdo != NULL) {
                struct lu_object *o;
		
                o = &mdo->mod_obj.mo_lu;
                lu_object_init(o, NULL, d);
                mdo->mod_obj.mo_ops = &mdd_obj_ops;
                mdo->mod_obj.mo_dir_ops = &mdd_dir_ops;
                o->lo_ops = &mdd_lu_obj_ops;
                return &mdo->mod_obj.mo_lu;
        } else
                return NULL;
}

static int mdd_object_init(struct lu_context *ctxt, struct lu_object *o)
{
	struct mdd_device *d = lu2mdd_dev(o->lo_dev);
	struct lu_object  *below;
        struct lu_device  *under;
        ENTRY;

	under = &d->mdd_child->dd_lu_dev;
	below = under->ld_ops->ldo_object_alloc(ctxt, under);

        if (below == NULL)
		RETURN(-ENOMEM);

        lu_object_add(o, below);
        RETURN(0);
}

static void mdd_object_free(struct lu_context *ctxt, struct lu_object *o)
{
	struct lu_object_header *h;
        struct mdd_object *mdd = mdd_obj(o);

	h = o->lo_header;
	lu_object_fini(o);
        OBD_FREE_PTR(mdd);
}

static int
mdd_attr_get(struct lu_context *ctxt,
             struct md_object *obj, struct lu_attr *attr)
{
        struct mdd_object *mdd_obj = mdo2mddo(obj);
        struct dt_object  *next = mdd_object_child(mdd_obj);
        int rc;

        ENTRY;

        rc = next->do_ops->do_attr_get(ctxt, next, attr);
        RETURN(rc);
}

static int
mdd_xattr_get(struct lu_context *ctxt, struct md_object *obj, void *buf,
              int buf_len, const char *name)
{
        struct mdd_object *mdd_obj = mdo2mddo(obj);
        struct dt_object  *next = mdd_object_child(mdd_obj);
        int rc;

        ENTRY;

        rc = next->do_ops->do_xattr_get(ctxt, next, buf, buf_len, name);
        RETURN(rc);
}

static int
__mdd_object_destroy(struct lu_context *ctxt, struct mdd_object *obj,
                     struct thandle *handle)
{
        struct dt_object  *next = mdd_object_child(obj);
        int rc;
        rc = next->do_ops->do_object_destroy(ctxt, next, handle);
        RETURN(rc);
}

static int mdd_add_orphan(struct mdd_device *mdd, struct mdd_object *obj,
                          struct thandle *handle)
{
        int rc = 0;
        ENTRY;

        RETURN(rc);
}

static int
open_orphan(struct mdd_object *obj)
{
        return 0;
}

static int
mdd_add_unlink_log(struct mdd_device *mdd, struct mdd_object *obj,
                   struct thandle *handle)
{
        return 0;
}

/*
 * number of blocks to reserve for particular operations. Should be function
 * of ... something. Stub for now.
 */
enum {
        MDD_OBJECT_DESTROY_CREDITS = 10,
        MDD_OBJECT_CREATE_CREDITS  = 10,
        MDD_ATTR_SET_CREDITS       = 10,
        MDD_XATTR_SET_CREDITS      = 10,
        MDD_INDEX_INSERT_CREDITS   = 10,
        MDD_INDEX_DELETE_CREDITS   = 10,
        MDD_LINK_CREDITS           = 10,
        MDD_RENAME_CREDITS         = 10,
        MDD_MKDIR_CREDITS          = 10
};

static int
mdd_object_destroy(struct lu_context *ctxt, struct md_object *obj)
{
        struct mdd_device *mdd = mdo2mdd(obj);
        struct mdd_object *mdd_obj = mdo2mddo(obj);
        struct thandle *handle;
        int rc ;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd,
                                 /*
                                  * TXN_PARAM should probably go into
                                  * lu_context_key to reduce stack
                                  * consumption. Currently this is just one
                                  * int, though.
                                  */
                                 &TXN_PARAM(MDD_OBJECT_DESTROY_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_lock(ctxt, mdd_obj, DT_WRITE_LOCK);
        if (open_orphan(mdd_obj))
                rc = mdd_add_orphan(mdd, mdd_obj, handle);
        else {
                rc = __mdd_object_destroy(ctxt, mdd_obj, handle);
                if (rc == 0)
                        rc = mdd_add_unlink_log(mdd, mdd_obj, handle);
        }

        mdd_unlock(ctxt, mdd_obj, DT_WRITE_LOCK);
        mdd_trans_stop(ctxt, mdd, handle);
        RETURN(rc);
}

static void mdd_object_release(struct lu_context *ctxt, struct lu_object *o)
{
}

static int mdd_object_exists(struct lu_context *ctx, struct lu_object *o)
{
        return lu_object_exists(ctx, lu_object_next(o));
}

static int mdd_object_print(struct lu_context *ctxt,
                            struct seq_file *f, const struct lu_object *o)
{
        return seq_printf(f, LUSTRE_MDD0_NAME"-object@%p", o);
}

static int mdd_fs_setup(struct mdd_device *mdd)
{
        return 0;
}

static int mdd_fs_cleanup(struct mdd_device *mdd)
{
        return 0;
}

static int mdd_device_init(struct lu_device *d, struct lu_device *next)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        int rc = -EFAULT;

        ENTRY;

        mdd->mdd_child = lu2dt_dev(next);

        rc = mdd_fs_setup(mdd);
        if (rc)
                GOTO(err, rc);

        RETURN(rc);
err:
        mdd_fs_cleanup(mdd);
        RETURN(rc);
}

static struct lu_device *mdd_device_fini(struct lu_device *d)
{
	struct mdd_device *m = lu2mdd_dev(d);
        struct lu_device *next = &m->mdd_child->dd_lu_dev;

        return next;
}

static int mdd_process_config(struct lu_device *d, struct lustre_cfg *cfg)
{
        struct mdd_device *m = lu2mdd_dev(d);
        struct lu_device *next = &m->mdd_child->dd_lu_dev;
        int err;

        switch(cfg->lcfg_command) {

        default:
                err = next->ld_ops->ldo_process_config(next, cfg);
        }

        RETURN(err);
}

static struct lu_device_operations mdd_lu_ops = {
	.ldo_object_alloc   = mdd_object_alloc,
	.ldo_object_free    = mdd_object_free,
        .ldo_process_config = mdd_process_config
};

static struct lu_object_operations mdd_lu_obj_ops = {
	.loo_object_init    = mdd_object_init,
	.loo_object_release = mdd_object_release,
	.loo_object_print   = mdd_object_print,
	.loo_object_exists  = mdd_object_exists
};

static struct dt_object* mdd_object_child(struct mdd_object *o)
{
        return container_of0(lu_object_next(&o->mod_obj.mo_lu),
                             struct dt_object, do_lu);
}

static void mdd_lock(struct lu_context *ctxt,
                     struct mdd_object *obj, enum dt_lock_mode mode)
{
        struct dt_object  *next = mdd_object_child(obj);

        next->do_ops->do_object_lock(ctxt, next, mode);
}

static void mdd_unlock(struct lu_context *ctxt,
                       struct mdd_object *obj, enum dt_lock_mode mode)
{
        struct dt_object  *next = mdd_object_child(obj);

        next->do_ops->do_object_unlock(ctxt, next, mode);
}

static void mdd_lock2(struct lu_context *ctxt,
                      struct mdd_object *o0, struct mdd_object *o1)
{
        mdd_lock(ctxt, o0, DT_WRITE_LOCK);
        mdd_lock(ctxt, o1, DT_WRITE_LOCK);
}

static void mdd_unlock2(struct lu_context *ctxt,
                        struct mdd_object *o0, struct mdd_object *o1)
{
        mdd_unlock(ctxt, o0, DT_WRITE_LOCK);
        mdd_unlock(ctxt, o1, DT_WRITE_LOCK);
}

static struct thandle* mdd_trans_start(struct lu_context *ctxt,
                                       struct mdd_device *mdd,
                                       struct txn_param *p)
{
        return mdd_child_ops(mdd)->dt_trans_start(ctxt, mdd->mdd_child, p);
}

static void mdd_trans_stop(struct lu_context *ctxt,
                           struct mdd_device *mdd, struct thandle *handle)
{
        mdd_child_ops(mdd)->dt_trans_stop(ctxt, handle);
}

static int
__mdd_object_create(struct lu_context *ctxt, struct mdd_object *obj,
                    struct lu_attr *attr, struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(obj);
        int rc;
        ENTRY;

        rc = next->do_ops->do_object_create(ctxt, next, attr, handle);
        /*XXX increase the refcount of the object or not?*/
        RETURN(rc);
}

static int mdd_object_create(struct lu_context *ctxt, struct md_object *obj,
                             struct lu_attr *attr)
{

        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int rc;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd,
                                 &TXN_PARAM(MDD_OBJECT_CREATE_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        rc = __mdd_object_create(ctxt, mdo2mddo(obj), attr, handle);

        mdd_trans_stop(ctxt, mdd, handle);

        RETURN(rc);
}


static int
__mdd_attr_set(struct lu_context *ctxt, struct md_object *obj,
               struct lu_attr *attr, struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(mdo2mddo(obj));
        return next->do_ops->do_attr_set(ctxt, next, attr, handle);
}

static int
mdd_attr_set(struct lu_context *ctxt,
             struct md_object *obj, struct lu_attr *attr)
{
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int  rc;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd, &TXN_PARAM(MDD_ATTR_SET_CREDITS));
        if (!handle)
                RETURN(-ENOMEM);

        rc = __mdd_attr_set(ctxt, obj, attr, handle);

        mdd_trans_stop(ctxt, mdd, handle);

        RETURN(rc);
}



static int
__mdd_xattr_set(struct lu_context *ctxt, struct mdd_device *mdd,
                struct mdd_object *obj, void *buf,
                int buf_len, const char *name, struct thandle *handle)
{
        struct dt_object *next = mdd_object_child(obj);
        return next->do_ops->do_xattr_set(ctxt, next, buf, buf_len,
                                          name, handle);
}

static int
mdd_xattr_set(struct lu_context *ctxt, struct md_object *obj, void *buf,
              int buf_len, const char *name)
{
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int  rc;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd, &TXN_PARAM(MDD_XATTR_SET_CREDITS));
        if (!handle)
                RETURN(-ENOMEM);

        rc = __mdd_xattr_set(ctxt, mdd, mdo2mddo(obj), buf, buf_len, name,
                             handle);

        mdd_trans_stop(ctxt, mdd, handle);

        RETURN(rc);
}

static const struct lu_fid *mdd_object_getfid(struct mdd_object *obj)
{
        return lu_object_fid(&obj->mod_obj.mo_lu);
}

static int
__mdd_index_insert(struct lu_context *ctxt, struct mdd_object *pobj,
                   const struct lu_fid *lf, const char *name,
                   struct thandle *handle)
{
        int rc;
        struct dt_object *next = mdd_object_child(pobj);

        rc = next->do_index_ops->dio_insert(ctxt, next, lf, name, handle);
        return rc;
}

static int
__mdd_index_delete(struct lu_context *ctxt, struct mdd_device *mdd,
                   struct mdd_object *pobj,
                   struct mdd_object *obj, const char *name,
                   struct thandle *handle)
{
        int rc;
        struct dt_object *next = mdd_object_child(pobj);
        ENTRY;

        mdd_lock2(ctxt, pobj, obj);

        rc = next->do_index_ops->dio_delete(ctxt, next, mdd_object_getfid(obj),
                                            name, handle);
        mdd_unlock2(ctxt, pobj, obj);

        RETURN(rc);
}

static int
mdd_index_delete(struct lu_context *ctxt, struct md_object *pobj,
                 struct md_object *obj, const char *name)
{
        struct mdd_object *mdd_pobj = mdo2mddo(pobj);
        struct mdd_object *mdd_obj = mdo2mddo(obj);
        struct mdd_device *mdd = mdo2mdd(obj);
        struct thandle *handle;
        int rc;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd,
                                 &TXN_PARAM(MDD_INDEX_DELETE_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        rc = __mdd_index_delete(ctxt, mdd, mdd_pobj, mdd_obj, name, handle);

        mdd_trans_stop(ctxt, mdd, handle);

        RETURN(rc);
}

static int
mdd_link(struct lu_context *ctxt, struct md_object *tgt_obj,
         struct md_object *src_obj, const char *name)
{
        struct mdd_object *mdd_tobj = mdo2mddo(tgt_obj);
        struct mdd_object *mdd_sobj = mdo2mddo(src_obj);
        struct mdd_device *mdd = mdo2mdd(src_obj);
        struct thandle *handle;
        int rc, nlink;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd, &TXN_PARAM(MDD_LINK_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_lock2(ctxt, mdd_tobj, mdd_sobj);

        rc = __mdd_index_insert(ctxt, mdd_tobj, lu_object_fid(&src_obj->mo_lu),
                                name, handle);
        if (rc)
                GOTO(exit, rc);

        rc = mdd_xattr_get(ctxt, src_obj, &nlink, sizeof(nlink), "NLINK");
        ++nlink;

        rc = __mdd_xattr_set(ctxt, mdd, mdd_sobj,
                             &nlink, sizeof(nlink), "NLINK", handle);
exit:
        mdd_unlock2(ctxt, mdd_tobj, mdd_sobj);

        mdd_trans_stop(ctxt, mdd, handle);
        RETURN(rc);
}

static void mdd_rename_lock(struct mdd_device *mdd, struct mdd_object *src_pobj,
                            struct mdd_object *tgt_pobj, struct mdd_object *sobj,
                            struct mdd_object *tobj)
{
        return;
}

static void mdd_rename_unlock(struct mdd_device *mdd, struct mdd_object *src_pobj,
                              struct mdd_object *tgt_pobj, struct mdd_object *sobj,
                              struct mdd_object *tobj)
{
        return;
}

static int
mdd_rename(struct lu_context *ctxt, struct md_object *src_pobj,
           struct md_object *tgt_pobj, struct md_object *sobj,
           const char *sname, struct md_object *tobj, const char *tname)
{
        struct mdd_device *mdd = mdo2mdd(src_pobj);
        struct mdd_object *mdd_spobj = mdo2mddo(src_pobj);
        struct mdd_object *mdd_tpobj = mdo2mddo(tgt_pobj);
        struct mdd_object *mdd_sobj = mdo2mddo(sobj);
        struct mdd_object *mdd_tobj = mdo2mddo(tobj);
        int rc;
        struct thandle *handle;

        handle = mdd_trans_start(ctxt, mdd, &TXN_PARAM(MDD_RENAME_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_rename_lock(mdd, mdd_spobj, mdd_tpobj, mdd_sobj, mdd_tobj);

        rc = __mdd_index_delete(ctxt, mdd, mdd_spobj, mdd_sobj, sname, handle);
        if (rc)
                GOTO(cleanup, rc);

        rc = __mdd_index_delete(ctxt, mdd, mdd_tpobj, mdd_tobj, tname, handle);
        if (rc)
                GOTO(cleanup, rc);

        rc = __mdd_index_insert(ctxt, mdd_spobj, lu_object_fid(&tobj->mo_lu),
                                tname, handle);
        if (rc)
                GOTO(cleanup, rc);

        /*
         * XXX nikita: huh? What is this?
         */
        rc = __mdd_object_destroy(ctxt, mdd_sobj, handle);
        if (rc)
                GOTO(cleanup, rc);
cleanup:
        mdd_rename_unlock(mdd, mdd_spobj, mdd_tpobj, mdd_sobj, mdd_tobj);
        mdd_trans_stop(ctxt, mdd, handle);
        RETURN(rc);
}

static int mdd_mkdir(struct lu_context *ctxt, struct lu_attr* attr,
                     struct md_object *pobj, const char *name,
                     struct md_object *child)
{
        struct mdd_device *mdd = mdo2mdd(pobj);
        struct mdd_object *mdo = mdo2mddo(pobj);
        struct thandle *handle;
        int rc = 0;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd, &TXN_PARAM(MDD_MKDIR_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_lock(ctxt, mdo, DT_WRITE_LOCK);

        rc = __mdd_object_create(ctxt, mdo2mddo(child), attr, handle);
        if (rc)
                GOTO(cleanup, rc);

        rc = __mdd_index_insert(ctxt, mdo, lu_object_fid(&child->mo_lu),
                                name, handle);
        if (rc)
                GOTO(cleanup, rc);
cleanup:
        mdd_unlock(ctxt, mdo, DT_WRITE_LOCK);
        mdd_trans_stop(ctxt, mdd, handle);
        RETURN(rc);
}

static int mdd_mkname(struct lu_context *ctxt, struct md_object *pobj,
          const char *name, const struct lu_fid *fid, struct lu_attr *attr)
{
        struct mdd_device *mdd = mdo2mdd(pobj);
        struct mdd_object *mdo = mdo2mddo(pobj);
        struct thandle *handle;
        int rc = 0;
        ENTRY;

        handle = mdd_trans_start(ctxt, mdd,
                                 &TXN_PARAM(MDD_INDEX_INSERT_CREDITS));
        if (IS_ERR(handle))
                RETURN(PTR_ERR(handle));

        mdd_lock(ctxt, mdo, DT_WRITE_LOCK);

        rc = __mdd_index_insert(ctxt, mdo, fid, name, handle);

        mdd_unlock(ctxt, mdo, DT_WRITE_LOCK);
        mdd_trans_stop(ctxt, mdd, handle);
        RETURN(rc);
}

static int mdd_root_get(struct lu_context *ctx,
                        struct md_device *m, struct lu_fid *f)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);

        ENTRY;
        RETURN(mdd_child_ops(mdd)->dt_root_get(ctx, mdd->mdd_child, f));
}

static int mdd_config(struct lu_context *ctx, struct md_device *m,
                      const char *name, void *buf, int size, int mode)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;
        ENTRY;

        rc = mdd_child_ops(mdd)->dt_config(ctx, mdd->mdd_child,
                                           name, buf, size, mode);
        RETURN(rc);
}

static int mdd_statfs(struct lu_context *ctx,
                      struct md_device *m, struct kstatfs *sfs) {
	struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;

        ENTRY;

        rc = mdd_child_ops(mdd)->dt_statfs(ctx, mdd->mdd_child, sfs);

        RETURN(rc);
}

struct md_device_operations mdd_ops = {
        .mdo_root_get       = mdd_root_get,
        .mdo_config         = mdd_config,
        .mdo_statfs         = mdd_statfs,
};

static struct md_dir_operations mdd_dir_ops = {
        .mdo_mkdir         = mdd_mkdir,
        .mdo_rename        = mdd_rename,
        .mdo_link          = mdd_link,
        .mdo_name_insert   = mdd_mkname
};

static struct md_object_operations mdd_obj_ops = {
        .moo_attr_get      = mdd_attr_get,
        .moo_attr_set      = mdd_attr_set,
        .moo_xattr_get     = mdd_xattr_get,
        .moo_xattr_set     = mdd_xattr_set,
        .moo_object_create  = mdd_object_create
};

static struct obd_ops mdd_obd_device_ops = {
        .o_owner = THIS_MODULE
};

struct lu_device *mdd_device_alloc(struct lu_device_type *t,
                                   struct lustre_cfg *lcfg)
{
        struct lu_device  *l;
        struct mdd_device *m;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                md_device_init(&m->mdd_md_dev, t);
                l = mdd2lu_dev(m);
	        l->ld_ops = &mdd_lu_ops;
                m->mdd_md_dev.md_ops = &mdd_ops;
        }

        return l;
}

void mdd_device_free(struct lu_device *lu)
{
        struct mdd_device *m = lu2mdd_dev(lu);

        LASSERT(atomic_read(&lu->ld_ref) == 0);
        md_device_fini(&m->mdd_md_dev);

        OBD_FREE_PTR(m);
}

int mdd_type_init(struct lu_device_type *t)
{
        return 0;
}

void mdd_type_fini(struct lu_device_type *t)
{
}

static struct lu_device_type_operations mdd_device_type_ops = {
        .ldto_init = mdd_type_init,
        .ldto_fini = mdd_type_fini,

        .ldto_device_alloc = mdd_device_alloc,
        .ldto_device_free  = mdd_device_free,

        .ldto_device_init    = mdd_device_init,
        .ldto_device_fini    = mdd_device_fini
};

static struct lu_device_type mdd_device_type = {
        .ldt_tags = LU_DEVICE_MD,
        .ldt_name = LUSTRE_MDD0_NAME,
        .ldt_ops  = &mdd_device_type_ops
};

struct lprocfs_vars lprocfs_mdd_obd_vars[] = {
        { 0 }
};

struct lprocfs_vars lprocfs_mdd_module_vars[] = {
        { 0 }
};

LPROCFS_INIT_VARS(mdd, lprocfs_mdd_module_vars, lprocfs_mdd_obd_vars);

static int __init mdd_mod_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(mdd, &lvars);
        return class_register_type(&mdd_obd_device_ops, NULL, lvars.module_vars,
                                   LUSTRE_MDD0_NAME, &mdd_device_type);
}

static void __exit mdd_mod_exit(void)
{
        class_unregister_type(LUSTRE_MDD0_NAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Meta-data Device Prototype ("LUSTRE_MDD0_NAME")");
MODULE_LICENSE("GPL");

cfs_module(mdd, "0.0.2", mdd_mod_init, mdd_mod_exit);
