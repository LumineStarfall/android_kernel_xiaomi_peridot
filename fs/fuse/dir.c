/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/fdtable.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/filter.h>
#include <linux/fs_context.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/iversion.h>
#include <linux/posix_acl.h>
#include <linux/security.h>
#include <linux/types.h>
#include <linux/kernel.h>

static bool __read_mostly allow_sys_admin_access;
module_param(allow_sys_admin_access, bool, 0644);
MODULE_PARM_DESC(allow_sys_admin_access,
		 "Allow users with CAP_SYS_ADMIN in initial userns to bypass allow_other access check");

#include "../internal.h"

static void fuse_advise_use_readdirplus(struct inode *dir)
{
	struct fuse_inode *fi = get_fuse_inode(dir);

	set_bit(FUSE_I_ADVISE_RDPLUS, &fi->state);
}

#if BITS_PER_LONG >= 64 && !defined(CONFIG_FUSE_BPF)
static inline void __fuse_dentry_settime(struct dentry *entry, u64 time)
{
	entry->d_fsdata = (void *) time;
}

static inline u64 fuse_dentry_time(const struct dentry *entry)
{
	return (u64)entry->d_fsdata;
}

#else

static inline void __fuse_dentry_settime(struct dentry *dentry, u64 time)
{
	((struct fuse_dentry *) dentry->d_fsdata)->time = time;
}

static inline u64 fuse_dentry_time(const struct dentry *entry)
{
	return ((struct fuse_dentry *) entry->d_fsdata)->time;
}
#endif

static void fuse_dentry_settime(struct dentry *dentry, u64 time)
{
	struct fuse_conn *fc = get_fuse_conn_super(dentry->d_sb);
	bool delete = !time && fc->delete_stale;
	/*
	 * Mess with DCACHE_OP_DELETE because dput() will be faster without it.
	 * Don't care about races, either way it's just an optimization
	 */
	if ((!delete && (dentry->d_flags & DCACHE_OP_DELETE)) ||
	    (delete && !(dentry->d_flags & DCACHE_OP_DELETE))) {
		spin_lock(&dentry->d_lock);
		if (!delete)
			dentry->d_flags &= ~DCACHE_OP_DELETE;
		else
			dentry->d_flags |= DCACHE_OP_DELETE;
		spin_unlock(&dentry->d_lock);
	}

	__fuse_dentry_settime(dentry, time);
}

void fuse_init_dentry_root(struct dentry *root, struct file *backing_dir)
{
#ifdef CONFIG_FUSE_BPF
	struct fuse_dentry *fuse_dentry = root->d_fsdata;

	if (backing_dir) {
		fuse_dentry->backing_path = backing_dir->f_path;
		path_get(&fuse_dentry->backing_path);
	}
#endif
}

/*
 * Set dentry and possibly attribute timeouts from the lookup/mk*
 * replies
 */
void fuse_change_entry_timeout(struct dentry *entry, struct fuse_entry_out *o)
{
	fuse_dentry_settime(entry,
		time_to_jiffies(o->entry_valid, o->entry_valid_nsec));
}

u64 entry_attr_timeout(struct fuse_entry_out *o)
{
	return time_to_jiffies(o->attr_valid, o->attr_valid_nsec);
}

void fuse_invalidate_attr_mask(struct inode *inode, u32 mask)
{
	set_mask_bits(&get_fuse_inode(inode)->inval_mask, 0, mask);
}

/*
 * Mark the attributes as stale, so that at the next call to
 * ->getattr() they will be fetched from userspace
 */
void fuse_invalidate_attr(struct inode *inode)
{
	fuse_invalidate_attr_mask(inode, STATX_BASIC_STATS);
}

static void fuse_dir_changed(struct inode *dir)
{
	fuse_invalidate_attr(dir);
	inode_maybe_inc_iversion(dir, false);
}

/**
 * Mark the attributes as stale due to an atime change.  Avoid the invalidate if
 * atime is not used.
 */
void fuse_invalidate_atime(struct inode *inode)
{
	if (!IS_RDONLY(inode))
		fuse_invalidate_attr_mask(inode, STATX_ATIME);
}

/*
 * Just mark the entry as stale, so that a next attempt to look it up
 * will result in a new lookup call to userspace
 *
 * This is called when a dentry is about to become negative and the
 * timeout is unknown (unlink, rmdir, rename and in some cases
 * lookup)
 */
void fuse_invalidate_entry_cache(struct dentry *entry)
{
	fuse_dentry_settime(entry, 0);
}

/*
 * Same as fuse_invalidate_entry_cache(), but also try to remove the
 * dentry from the hash
 */
static void fuse_invalidate_entry(struct dentry *entry)
{
	d_invalidate(entry);
	fuse_invalidate_entry_cache(entry);
}

static void fuse_lookup_init(struct fuse_conn *fc, struct fuse_args *args,
			     u64 nodeid, const struct qstr *name,
			     struct fuse_entry_out *outarg,
			     struct fuse_entry_bpf_out *bpf_outarg)
{
	memset(outarg, 0, sizeof(struct fuse_entry_out));
	args->opcode = FUSE_LOOKUP;
	args->nodeid = nodeid;
	args->in_numargs = 1;
	args->in_args[0].size = name->len + 1;
	args->in_args[0].value = name->name;
	args->out_argvar = true;
	args->out_numargs = 2;
	args->out_args[0].size = sizeof(struct fuse_entry_out);
	args->out_args[0].value = outarg;
	args->out_args[1].size = sizeof(struct fuse_entry_bpf_out);
	args->out_args[1].value = bpf_outarg;
}

#ifdef CONFIG_FUSE_BPF
static bool backing_data_changed(struct fuse_inode *fi, struct dentry *entry,
				 struct fuse_entry_bpf *bpf_arg)
{
	struct path new_backing_path;
	struct inode *new_backing_inode;
	struct bpf_prog *bpf = NULL;
	int err;
	bool ret = true;

	if (!entry || !fi->backing_inode) {
		ret = false;
		goto put_backing_file;
	}

	get_fuse_backing_path(entry, &new_backing_path);
	new_backing_inode = fi->backing_inode;
	ihold(new_backing_inode);

	err = fuse_handle_backing(bpf_arg, &new_backing_inode, &new_backing_path);

	if (err)
		goto put_inode;

	err = fuse_handle_bpf_prog(bpf_arg, entry->d_parent->d_inode, &bpf);
	if (err)
		goto put_bpf;

	ret = (bpf != fi->bpf || fi->backing_inode != new_backing_inode ||
			!path_equal(&get_fuse_dentry(entry)->backing_path, &new_backing_path));
put_bpf:
	if (bpf)
		bpf_prog_put(bpf);
put_inode:
	iput(new_backing_inode);
	path_put(&new_backing_path);
put_backing_file:
	if (bpf_arg->backing_file)
		fput(bpf_arg->backing_file);
	return ret;
}
#endif

/*
 * Check whether the dentry is still valid
 *
 * If the entry validity timeout has expired and the dentry is
 * positive, try to redo the lookup.  If the lookup results in a
 * different inode, then let the VFS invalidate the dentry and redo
 * the lookup once more.  If the lookup results in the same inode,
 * then refresh the attributes, timeouts and mark the dentry valid.
 */
static int fuse_dentry_revalidate(struct dentry *entry, unsigned int flags)
{
	struct inode *inode;
	struct dentry *parent;
	struct fuse_mount *fm;
	struct fuse_inode *fi;
	int ret;

	inode = d_inode_rcu(entry);
	if (inode && fuse_is_bad(inode))
		goto invalid;

#ifdef CONFIG_FUSE_BPF
	/* TODO: Do we need bpf support for revalidate?
	 * If the lower filesystem says the entry is invalid, FUSE probably shouldn't
	 * try to fix that without going through the normal lookup path...
	 */
	if (get_fuse_dentry(entry)->backing_path.dentry) {
		ret = fuse_revalidate_backing(entry, flags);
		if (ret <= 0) {
			goto out;
		}
	}
#endif
	if (time_before64(fuse_dentry_time(entry), get_jiffies_64()) ||
		 (flags & (LOOKUP_EXCL | LOOKUP_REVAL | LOOKUP_RENAME_TARGET))) {
		struct fuse_entry_out outarg;
		struct fuse_entry_bpf bpf_arg;
		FUSE_ARGS(args);
		struct fuse_forget_link *forget;
		u64 attr_version;

		/* For negative dentries, always do a fresh lookup */
		if (!inode)
			goto invalid;

		ret = -ECHILD;
		if (flags & LOOKUP_RCU)
			goto out;
		fm = get_fuse_mount(inode);

		parent = dget_parent(entry);

#ifdef CONFIG_FUSE_BPF
		/* TODO: Once we're handling timeouts for backing inodes, do a
		 * bpf based lookup_revalidate here.
		 */
		if (get_fuse_inode(parent->d_inode)->backing_inode) {
			dput(parent);
			ret = 1;
			goto out;
		}
#endif
		forget = fuse_alloc_forget();
		ret = -ENOMEM;
		if (!forget) {
			dput(parent);
			goto out;
		}

		attr_version = fuse_get_attr_version(fm->fc);

		fuse_lookup_init(fm->fc, &args, get_node_id(d_inode(parent)),
				 &entry->d_name, &outarg, &bpf_arg.out);
		ret = fuse_simple_request(fm, &args);
		dput(parent);

		/* Zero nodeid is same as -ENOENT */
		if (!ret && !outarg.nodeid)
			ret = -ENOENT;
		if (!ret || ret == sizeof(bpf_arg.out)) {
			fi = get_fuse_inode(inode);
			if (outarg.nodeid != get_node_id(inode) ||
#ifdef CONFIG_FUSE_BPF
			    (ret == sizeof(bpf_arg.out) &&
					    backing_data_changed(fi, entry, &bpf_arg)) ||
#endif
			    (bool) IS_AUTOMOUNT(inode) != (bool) (outarg.attr.flags & FUSE_ATTR_SUBMOUNT)) {
				fuse_queue_forget(fm->fc, forget,
						  outarg.nodeid, 1);
				goto invalid;
			}
			spin_lock(&fi->lock);
			fi->nlookup++;
			spin_unlock(&fi->lock);
		}
		kfree(forget);
		if (ret == -ENOMEM || ret == -EINTR)
			goto out;
		if (ret || fuse_invalid_attr(&outarg.attr) ||
		    fuse_stale_inode(inode, outarg.generation, &outarg.attr))
			goto invalid;

		forget_all_cached_acls(inode);
		fuse_change_attributes(inode, &outarg.attr,
				       entry_attr_timeout(&outarg),
				       attr_version);
		fuse_change_entry_timeout(entry, &outarg);
	} else if (inode) {
		fi = get_fuse_inode(inode);
		if (flags & LOOKUP_RCU) {
			if (test_bit(FUSE_I_INIT_RDPLUS, &fi->state))
				return -ECHILD;
		} else if (test_and_clear_bit(FUSE_I_INIT_RDPLUS, &fi->state)) {
			parent = dget_parent(entry);
			fuse_advise_use_readdirplus(d_inode(parent));
			dput(parent);
		}
	}
	ret = 1;
out:
	return ret;

invalid:
	ret = 0;
	goto out;
}

#if BITS_PER_LONG < 64 || defined(CONFIG_FUSE_BPF)
static int fuse_dentry_init(struct dentry *dentry)
{
	dentry->d_fsdata = kzalloc(sizeof(struct fuse_dentry),
				   GFP_KERNEL_ACCOUNT | __GFP_RECLAIMABLE);

	return dentry->d_fsdata ? 0 : -ENOMEM;
}
static void fuse_dentry_release(struct dentry *dentry)
{
	struct fuse_dentry *fd = dentry->d_fsdata;

#ifdef CONFIG_FUSE_BPF
	if (fd && fd->backing_path.dentry)
		path_put(&fd->backing_path);

	if (fd && fd->bpf)
		bpf_prog_put(fd->bpf);
#endif

	kfree_rcu(fd, rcu);
}
#endif

static int fuse_dentry_delete(const struct dentry *dentry)
{
	return time_before64(fuse_dentry_time(dentry), get_jiffies_64());
}

/*
 * Create a fuse_mount object with a new superblock (with path->dentry
 * as the root), and return that mount so it can be auto-mounted on
 * @path.
 */
static struct vfsmount *fuse_dentry_automount(struct path *path)
{
	struct fs_context *fsc;
	struct vfsmount *mnt;
	struct fuse_inode *mp_fi = get_fuse_inode(d_inode(path->dentry));

	fsc = fs_context_for_submount(path->mnt->mnt_sb->s_type, path->dentry);
	if (IS_ERR(fsc))
		return ERR_CAST(fsc);

	/* Pass the FUSE inode of the mount for fuse_get_tree_submount() */
	fsc->fs_private = mp_fi;

	/* Create the submount */
	mnt = fc_mount(fsc);
	if (!IS_ERR(mnt))
		mntget(mnt);

	put_fs_context(fsc);
	return mnt;
}

/*
 * Get the canonical path. Since we must translate to a path, this must be done
 * in the context of the userspace daemon, however, the userspace daemon cannot
 * look up paths on its own. Instead, we handle the lookup as a special case
 * inside of the write request.
 */
static int fuse_dentry_canonical_path(const struct path *path,
				       struct path *canonical_path)
{
	struct inode *inode = d_inode(path->dentry);
	//struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_mount *fm = get_fuse_mount_super(path->mnt->mnt_sb);
	FUSE_ARGS(args);
	char *path_name;
	int err;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;
#endif

	if (fm->fc->no_dentry_canonical_path)
		goto out;

#ifdef CONFIG_FUSE_BPF
	fer = fuse_bpf_backing(inode, struct fuse_dummy_io,
			       fuse_canonical_path_initialize,
			       fuse_canonical_path_backing,
			       fuse_canonical_path_finalize, path,
			       canonical_path);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	path_name = (char *)get_zeroed_page(GFP_KERNEL);
	if (!path_name)
		return -ENOMEM;

	args.opcode = FUSE_CANONICAL_PATH;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 0;
	args.out_numargs = 1;
	args.out_args[0].size = PATH_MAX;
	args.out_args[0].value = path_name;
	args.canonical_path = canonical_path;
	args.out_argvar = 1;

	err = fuse_simple_request(fm, &args);
	free_page((unsigned long)path_name);
	if (err > 0)
		return 0;
	if (err < 0 && err != -ENOSYS)
		return err;

	if (err == -ENOSYS)
		fm->fc->no_dentry_canonical_path = 1;

out:
	canonical_path->dentry = path->dentry;
	canonical_path->mnt = path->mnt;
	path_get(canonical_path);
	return 0;
}

const struct dentry_operations fuse_dentry_operations = {
	.d_revalidate	= fuse_dentry_revalidate,
	.d_delete	= fuse_dentry_delete,
#if BITS_PER_LONG < 64 || defined(CONFIG_FUSE_BPF)
	.d_init		= fuse_dentry_init,
	.d_release	= fuse_dentry_release,
#endif
	.d_automount	= fuse_dentry_automount,
	.d_canonical_path = fuse_dentry_canonical_path,
};

const struct dentry_operations fuse_root_dentry_operations = {
#if BITS_PER_LONG < 64 || defined(CONFIG_FUSE_BPF)
	.d_init		= fuse_dentry_init,
	.d_release	= fuse_dentry_release,
#endif
};

int fuse_valid_type(int m)
{
	return S_ISREG(m) || S_ISDIR(m) || S_ISLNK(m) || S_ISCHR(m) ||
		S_ISBLK(m) || S_ISFIFO(m) || S_ISSOCK(m);
}

bool fuse_invalid_attr(struct fuse_attr *attr)
{
	return !fuse_valid_type(attr->mode) ||
		attr->size > LLONG_MAX;
}

int fuse_lookup_name(struct super_block *sb, u64 nodeid, const struct qstr *name,
		     struct fuse_entry_out *outarg,
		     struct dentry *entry,
		     struct inode **inode)
{
	struct fuse_mount *fm = get_fuse_mount_super(sb);
	FUSE_ARGS(args);
	struct fuse_entry_bpf bpf_arg = {0};
	struct fuse_forget_link *forget;
	u64 attr_version;
	int err;

	*inode = NULL;
	err = -ENAMETOOLONG;
	if (name->len > FUSE_NAME_MAX)
		goto out;

	forget = fuse_alloc_forget();
	err = -ENOMEM;
	if (!forget)
		goto out;

	attr_version = fuse_get_attr_version(fm->fc);

	fuse_lookup_init(fm->fc, &args, nodeid, name, outarg, &bpf_arg.out);
	err = fuse_simple_request(fm, &args);

#ifdef CONFIG_FUSE_BPF
	if (err == sizeof(bpf_arg.out)) {
		/* TODO Make sure this handles invalid handles */
		struct file *backing_file;
		struct inode *backing_inode;

		err = -ENOENT;
		if (!entry)
			goto out_put_forget;

		err = -EINVAL;
		backing_file = bpf_arg.backing_file;
		if (!backing_file)
			goto out_put_forget;

		if (IS_ERR(backing_file)) {
			err = PTR_ERR(backing_file);
			goto out_put_forget;
		}

		backing_inode = backing_file->f_inode;
		*inode = fuse_iget_backing(sb, outarg->nodeid, backing_inode);
		if (!*inode)
			goto out_put_forget;

		err = fuse_handle_backing(&bpf_arg,
				&get_fuse_inode(*inode)->backing_inode,
				&get_fuse_dentry(entry)->backing_path);
		if (!err)
			err = fuse_handle_bpf_prog(&bpf_arg, NULL,
					   &get_fuse_inode(*inode)->bpf);
		if (err) {
			iput(*inode);
			*inode = NULL;
			goto out_put_forget;
		}
	} else
#endif
	{
		/* Zero nodeid is same as -ENOENT, but with valid timeout */
		if (err || !outarg->nodeid)
			goto out_put_forget;

		err = -EIO;
		if (!outarg->nodeid)
			goto out_put_forget;
		if (fuse_invalid_attr(&outarg->attr))
			goto out_put_forget;
		if (outarg->nodeid == FUSE_ROOT_ID && outarg->generation != 0) {
			pr_warn_once("root generation should be zero\n");
			outarg->generation = 0;
		}

		*inode = fuse_iget(sb, outarg->nodeid, outarg->generation,
				   &outarg->attr, entry_attr_timeout(outarg),
				   attr_version);
	}

	err = -ENOMEM;
	if (!*inode && outarg->nodeid) {
		fuse_queue_forget(fm->fc, forget, outarg->nodeid, 1);
		goto out;
	}
	err = 0;

 out_put_forget:
	kfree(forget);
 out:
	if (bpf_arg.backing_file)
		fput(bpf_arg.backing_file);
	return err;
}

static struct dentry *fuse_lookup(struct inode *dir, struct dentry *entry,
				  unsigned int flags)
{
	int err;
	struct fuse_entry_out outarg;
	struct inode *inode;
	struct dentry *newent;
	bool outarg_valid = true;
	bool locked;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(dir, struct fuse_lookup_io,
			       fuse_lookup_initialize, fuse_lookup_backing,
			       fuse_lookup_finalize,
			       dir, entry, flags);
	if (fer.ret)
		return fer.result;
#endif

	if (fuse_is_bad(dir))
		return ERR_PTR(-EIO);

	locked = fuse_lock_inode(dir);
	err = fuse_lookup_name(dir->i_sb, get_node_id(dir), &entry->d_name,
			       &outarg, entry, &inode);
	fuse_unlock_inode(dir, locked);
	if (err == -ENOENT) {
		outarg_valid = false;
		err = 0;
	}
	if (err)
		goto out_err;

	err = -EIO;
	if (inode && get_node_id(inode) == FUSE_ROOT_ID)
		goto out_iput;

	newent = d_splice_alias(inode, entry);
	err = PTR_ERR(newent);
	if (IS_ERR(newent))
		goto out_err;

	entry = newent ? newent : entry;
	if (outarg_valid)
		fuse_change_entry_timeout(entry, &outarg);
	else
		fuse_invalidate_entry_cache(entry);

	if (inode)
		fuse_advise_use_readdirplus(dir);
	return newent;

 out_iput:
	iput(inode);
 out_err:
	return ERR_PTR(err);
}

static int get_security_context(struct dentry *entry, umode_t mode,
				struct fuse_in_arg *ext)
{
	struct fuse_secctx *fctx;
	struct fuse_secctx_header *header;
	void *ctx = NULL, *ptr;
	u32 ctxlen, total_len = sizeof(*header);
	int err, nr_ctx = 0;
	const char *name;
	size_t namelen;

	err = security_dentry_init_security(entry, mode, &entry->d_name,
					    &name, &ctx, &ctxlen);
	if (err) {
		if (err != -EOPNOTSUPP)
			goto out_err;
		/* No LSM is supporting this security hook. Ignore error */
		ctxlen = 0;
		ctx = NULL;
	}

	if (ctxlen) {
		nr_ctx = 1;
		namelen = strlen(name) + 1;
		err = -EIO;
		if (WARN_ON(namelen > XATTR_NAME_MAX + 1 || ctxlen > S32_MAX))
			goto out_err;
		total_len += FUSE_REC_ALIGN(sizeof(*fctx) + namelen + ctxlen);
	}

	err = -ENOMEM;
	header = ptr = kzalloc(total_len, GFP_KERNEL);
	if (!ptr)
		goto out_err;

	header->nr_secctx = nr_ctx;
	header->size = total_len;
	ptr += sizeof(*header);
	if (nr_ctx) {
		fctx = ptr;
		fctx->size = ctxlen;
		ptr += sizeof(*fctx);

		strcpy(ptr, name);
		ptr += namelen;

		memcpy(ptr, ctx, ctxlen);
	}
	ext->size = total_len;
	ext->value = header;
	err = 0;
out_err:
	kfree(ctx);
	return err;
}

static int get_create_ext(struct fuse_args *args, struct dentry *dentry,
			  umode_t mode)
{
	struct fuse_conn *fc = get_fuse_conn_super(dentry->d_sb);
	struct fuse_in_arg ext = { .size = 0, .value = NULL };
	int err = 0;

	if (fc->init_security)
		err = get_security_context(dentry, mode, &ext);

	if (!err && ext.size) {
		WARN_ON(args->in_numargs >= ARRAY_SIZE(args->in_args));
		args->is_ext = true;
		args->ext_idx = args->in_numargs++;
		args->in_args[args->ext_idx] = ext;
	} else {
		kfree(ext.value);
	}

	return err;
}

static void free_ext_value(struct fuse_args *args)
{
	if (args->is_ext)
		kfree(args->in_args[args->ext_idx].value);
}

/*
 * Atomic create+open operation
 *
 * If the filesystem doesn't support this, then fall back to separate
 * 'mknod' + 'open' requests.
 */
static int fuse_create_open(struct inode *dir, struct dentry *entry,
			    struct file *file, unsigned int flags,
			    umode_t mode, u32 opcode)
{
	int err;
	struct inode *inode;
	struct fuse_conn *fc = get_fuse_conn(dir);
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);
	struct fuse_forget_link *forget;
	struct fuse_create_in inarg;
	struct fuse_open_out outopen;
	struct fuse_entry_out outentry;
	struct fuse_inode *fi;
	struct fuse_file *ff;
	bool trunc = flags & O_TRUNC;

	/* Userspace expects S_IFREG in create mode */
	BUG_ON((mode & S_IFMT) != S_IFREG);

#ifdef CONFIG_FUSE_BPF
	{
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(dir, struct fuse_create_open_io,
				       fuse_create_open_initialize,
				       fuse_create_open_backing,
				       fuse_create_open_finalize,
				       dir, entry, file, flags, mode);
		if (fer.ret)
			return PTR_ERR(fer.result);
	}
#endif

	forget = fuse_alloc_forget();
	err = -ENOMEM;
	if (!forget)
		goto out_err;

	err = -ENOMEM;
	ff = fuse_file_alloc(fm);
	if (!ff)
		goto out_put_forget_req;

	if (!fm->fc->dont_mask)
		mode &= ~current_umask();

	flags &= ~O_NOCTTY;
	memset(&inarg, 0, sizeof(inarg));
	memset(&outentry, 0, sizeof(outentry));
	inarg.flags = flags;
	inarg.mode = mode;
	inarg.umask = current_umask();

	if (fm->fc->handle_killpriv_v2 && trunc &&
	    !(flags & O_EXCL) && !capable(CAP_FSETID)) {
		inarg.open_flags |= FUSE_OPEN_KILL_SUIDGID;
	}

	args.opcode = opcode;
	args.nodeid = get_node_id(dir);
	args.in_numargs = 2;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.in_args[1].size = entry->d_name.len + 1;
	args.in_args[1].value = entry->d_name.name;
	args.out_numargs = 2;
	args.out_args[0].size = sizeof(outentry);
	args.out_args[0].value = &outentry;
	args.out_args[1].size = sizeof(outopen);
	args.out_args[1].value = &outopen;

	err = get_create_ext(&args, entry, mode);
	if (err)
		goto out_free_ff;

	err = fuse_simple_request(fm, &args);
	free_ext_value(&args);
	if (err)
		goto out_free_ff;

	err = -EIO;
	if (!S_ISREG(outentry.attr.mode) || invalid_nodeid(outentry.nodeid) ||
	    fuse_invalid_attr(&outentry.attr))
		goto out_free_ff;

	ff->fh = outopen.fh;
	ff->nodeid = outentry.nodeid;
	ff->open_flags = outopen.open_flags;
	fuse_passthrough_setup(fc, ff, &outopen);
	inode = fuse_iget(dir->i_sb, outentry.nodeid, outentry.generation,
			  &outentry.attr, entry_attr_timeout(&outentry), 0);
	if (!inode) {
		flags &= ~(O_CREAT | O_EXCL | O_TRUNC);
		fuse_sync_release(NULL, ff, flags);
		fuse_queue_forget(fm->fc, forget, outentry.nodeid, 1);
		err = -ENOMEM;
		goto out_err;
	}
	kfree(forget);
	d_instantiate(entry, inode);
	fuse_change_entry_timeout(entry, &outentry);
	fuse_dir_changed(dir);
	err = finish_open(file, entry, generic_file_open);
	if (err) {
		fi = get_fuse_inode(inode);
		fuse_sync_release(fi, ff, flags);
	} else {
		file->private_data = ff;
		fuse_finish_open(inode, file);
		if (fm->fc->atomic_o_trunc && trunc)
			truncate_pagecache(inode, 0);
		else if (!(ff->open_flags & FOPEN_KEEP_CACHE))
			invalidate_inode_pages2(inode->i_mapping);
	}
	return err;

out_free_ff:
	fuse_file_free(ff);
out_put_forget_req:
	kfree(forget);
out_err:
	return err;
}

static int fuse_mknod(struct user_namespace *, struct inode *, struct dentry *,
		      umode_t, dev_t);
static int fuse_atomic_open(struct inode *dir, struct dentry *entry,
			    struct file *file, unsigned flags,
			    umode_t mode)
{
	int err;
	struct fuse_conn *fc = get_fuse_conn(dir);
	struct dentry *res = NULL;

	if (fuse_is_bad(dir))
		return -EIO;

	if (d_in_lookup(entry)) {
		res = fuse_lookup(dir, entry, 0);
		if (IS_ERR(res))
			return PTR_ERR(res);

		if (res)
			entry = res;
	}

	if (!(flags & O_CREAT) || d_really_is_positive(entry))
		goto no_open;

	/* Only creates */
	file->f_mode |= FMODE_CREATED;

	if (fc->no_create)
		goto mknod;

	err = fuse_create_open(dir, entry, file, flags, mode, FUSE_CREATE);
	if (err == -ENOSYS) {
		fc->no_create = 1;
		goto mknod;
	}
out_dput:
	dput(res);
	return err;

mknod:
	err = fuse_mknod(&init_user_ns, dir, entry, mode, 0);
	if (err)
		goto out_dput;
no_open:
	return finish_no_open(file, res);
}

/*
 * Code shared between mknod, mkdir, symlink and link
 */
static int create_new_entry(struct fuse_mount *fm, struct fuse_args *args,
			    struct inode *dir, struct dentry *entry,
			    umode_t mode)
{
	struct fuse_entry_out outarg;
	struct inode *inode;
	struct dentry *d;
	int err;
	struct fuse_forget_link *forget;

	if (fuse_is_bad(dir))
		return -EIO;

	forget = fuse_alloc_forget();
	if (!forget)
		return -ENOMEM;

	memset(&outarg, 0, sizeof(outarg));
	args->nodeid = get_node_id(dir);
	args->out_numargs = 1;
	args->out_args[0].size = sizeof(outarg);
	args->out_args[0].value = &outarg;

	if (args->opcode != FUSE_LINK) {
		err = get_create_ext(args, entry, mode);
		if (err)
			goto out_put_forget_req;
	}

	err = fuse_simple_request(fm, args);
	free_ext_value(args);
	if (err)
		goto out_put_forget_req;

	err = -EIO;
	if (invalid_nodeid(outarg.nodeid) || fuse_invalid_attr(&outarg.attr))
		goto out_put_forget_req;

	if ((outarg.attr.mode ^ mode) & S_IFMT)
		goto out_put_forget_req;

	inode = fuse_iget(dir->i_sb, outarg.nodeid, outarg.generation,
			  &outarg.attr, entry_attr_timeout(&outarg), 0);
	if (!inode) {
		fuse_queue_forget(fm->fc, forget, outarg.nodeid, 1);
		return -ENOMEM;
	}
	kfree(forget);

	d_drop(entry);
	d = d_splice_alias(inode, entry);
	if (IS_ERR(d))
		return PTR_ERR(d);

	if (d) {
		fuse_change_entry_timeout(d, &outarg);
		dput(d);
	} else {
		fuse_change_entry_timeout(entry, &outarg);
	}
	fuse_dir_changed(dir);
	return 0;

 out_put_forget_req:
	kfree(forget);
	return err;
}

static int fuse_mknod(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *entry, umode_t mode, dev_t rdev)
{
	struct fuse_mknod_in inarg;
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(dir, struct fuse_mknod_in,
			fuse_mknod_initialize, fuse_mknod_backing,
			fuse_mknod_finalize,
			dir, entry, mode, rdev);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	if (!fm->fc->dont_mask)
		mode &= ~current_umask();

	memset(&inarg, 0, sizeof(inarg));
	inarg.mode = mode;
	inarg.rdev = new_encode_dev(rdev);
	inarg.umask = current_umask();
	args.opcode = FUSE_MKNOD;
	args.in_numargs = 2;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.in_args[1].size = entry->d_name.len + 1;
	args.in_args[1].value = entry->d_name.name;
	return create_new_entry(fm, &args, dir, entry, mode);
}

static int fuse_create(struct user_namespace *mnt_userns, struct inode *dir,
		       struct dentry *entry, umode_t mode, bool excl)
{
	return fuse_mknod(&init_user_ns, dir, entry, mode, 0);
}

static int fuse_tmpfile(struct user_namespace *mnt_userns, struct inode *dir,
			struct file *file, umode_t mode)
{
	struct fuse_conn *fc = get_fuse_conn(dir);
	int err;

	if (fc->no_tmpfile)
		return -EOPNOTSUPP;

	err = fuse_create_open(dir, file->f_path.dentry, file, file->f_flags, mode, FUSE_TMPFILE);
	if (err == -ENOSYS) {
		fc->no_tmpfile = 1;
		err = -EOPNOTSUPP;
	}
	return err;
}

static int fuse_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
		      struct dentry *entry, umode_t mode)
{
	struct fuse_mkdir_in inarg;
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(dir, struct fuse_mkdir_in,
			fuse_mkdir_initialize, fuse_mkdir_backing,
			fuse_mkdir_finalize,
			dir, entry, mode);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	if (!fm->fc->dont_mask)
		mode &= ~current_umask();

	memset(&inarg, 0, sizeof(inarg));
	inarg.mode = mode;
	inarg.umask = current_umask();
	args.opcode = FUSE_MKDIR;
	args.in_numargs = 2;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.in_args[1].size = entry->d_name.len + 1;
	args.in_args[1].value = entry->d_name.name;
	return create_new_entry(fm, &args, dir, entry, S_IFDIR);
}

static int fuse_symlink(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *entry, const char *link)
{
	struct fuse_mount *fm = get_fuse_mount(dir);
	unsigned len = strlen(link) + 1;
	FUSE_ARGS(args);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(dir, struct fuse_dummy_io,
			fuse_symlink_initialize, fuse_symlink_backing,
			fuse_symlink_finalize,
			dir, entry, link, len);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	args.opcode = FUSE_SYMLINK;
	args.in_numargs = 2;
	args.in_args[0].size = entry->d_name.len + 1;
	args.in_args[0].value = entry->d_name.name;
	args.in_args[1].size = len;
	args.in_args[1].value = link;
	return create_new_entry(fm, &args, dir, entry, S_IFLNK);
}

void fuse_flush_time_update(struct inode *inode)
{
	int err = sync_inode_metadata(inode, 1);

	mapping_set_error(inode->i_mapping, err);
}

static void fuse_update_ctime_in_cache(struct inode *inode)
{
	if (!IS_NOCMTIME(inode)) {
		inode->i_ctime = current_time(inode);
		mark_inode_dirty_sync(inode);
		fuse_flush_time_update(inode);
	}
}

void fuse_update_ctime(struct inode *inode)
{
	fuse_invalidate_attr_mask(inode, STATX_CTIME);
	fuse_update_ctime_in_cache(inode);
}

static void fuse_entry_unlinked(struct dentry *entry)
{
	struct inode *inode = d_inode(entry);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);

	spin_lock(&fi->lock);
	fi->attr_version = atomic64_inc_return(&fc->attr_version);
	/*
	 * If i_nlink == 0 then unlink doesn't make sense, yet this can
	 * happen if userspace filesystem is careless.  It would be
	 * difficult to enforce correct nlink usage so just ignore this
	 * condition here
	 */
	if (S_ISDIR(inode->i_mode))
		clear_nlink(inode);
	else if (inode->i_nlink > 0)
		drop_nlink(inode);
	spin_unlock(&fi->lock);
	fuse_invalidate_entry_cache(entry);
	fuse_update_ctime(inode);
}

static int fuse_unlink(struct inode *dir, struct dentry *entry)
{
	int err;
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);

	if (fuse_is_bad(dir))
		return -EIO;

#ifdef CONFIG_FUSE_BPF
	{
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(dir, struct fuse_dummy_io,
					fuse_unlink_initialize,
					fuse_unlink_backing,
					fuse_unlink_finalize,
					dir, entry);
		if (fer.ret)
			return PTR_ERR(fer.result);
	}
#endif

	args.opcode = FUSE_UNLINK;
	args.nodeid = get_node_id(dir);
	args.in_numargs = 1;
	args.in_args[0].size = entry->d_name.len + 1;
	args.in_args[0].value = entry->d_name.name;
	err = fuse_simple_request(fm, &args);
	if (!err) {
		fuse_dir_changed(dir);
		fuse_entry_unlinked(entry);
	} else if (err == -EINTR)
		fuse_invalidate_entry(entry);
	return err;
}

static int fuse_rmdir(struct inode *dir, struct dentry *entry)
{
	int err;
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);

	if (fuse_is_bad(dir))
		return -EIO;

#ifdef CONFIG_FUSE_BPF
	{
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(dir, struct fuse_dummy_io,
					fuse_rmdir_initialize,
					fuse_rmdir_backing,
					fuse_rmdir_finalize,
					dir, entry);
		if (fer.ret)
			return PTR_ERR(fer.result);
	}
#endif

	args.opcode = FUSE_RMDIR;
	args.nodeid = get_node_id(dir);
	args.in_numargs = 1;
	args.in_args[0].size = entry->d_name.len + 1;
	args.in_args[0].value = entry->d_name.name;
	err = fuse_simple_request(fm, &args);
	if (!err) {
		fuse_dir_changed(dir);
		fuse_entry_unlinked(entry);
	} else if (err == -EINTR)
		fuse_invalidate_entry(entry);
	return err;
}

static int fuse_rename_common(struct inode *olddir, struct dentry *oldent,
			      struct inode *newdir, struct dentry *newent,
			      unsigned int flags, int opcode, size_t argsize)
{
	int err;
	struct fuse_rename2_in inarg;
	struct fuse_mount *fm = get_fuse_mount(olddir);
	FUSE_ARGS(args);

	memset(&inarg, 0, argsize);
	inarg.newdir = get_node_id(newdir);
	inarg.flags = flags;
	args.opcode = opcode;
	args.nodeid = get_node_id(olddir);
	args.in_numargs = 3;
	args.in_args[0].size = argsize;
	args.in_args[0].value = &inarg;
	args.in_args[1].size = oldent->d_name.len + 1;
	args.in_args[1].value = oldent->d_name.name;
	args.in_args[2].size = newent->d_name.len + 1;
	args.in_args[2].value = newent->d_name.name;
	err = fuse_simple_request(fm, &args);
	if (!err) {
		/* ctime changes */
		fuse_update_ctime(d_inode(oldent));

		if (flags & RENAME_EXCHANGE)
			fuse_update_ctime(d_inode(newent));

		fuse_dir_changed(olddir);
		if (olddir != newdir)
			fuse_dir_changed(newdir);

		/* newent will end up negative */
		if (!(flags & RENAME_EXCHANGE) && d_really_is_positive(newent))
			fuse_entry_unlinked(newent);
	} else if (err == -EINTR) {
		/* If request was interrupted, DEITY only knows if the
		   rename actually took place.  If the invalidation
		   fails (e.g. some process has CWD under the renamed
		   directory), then there can be inconsistency between
		   the dcache and the real filesystem.  Tough luck. */
		fuse_invalidate_entry(oldent);
		if (d_really_is_positive(newent))
			fuse_invalidate_entry(newent);
	}

	return err;
}

static int fuse_rename2(struct user_namespace *mnt_userns, struct inode *olddir,
			struct dentry *oldent, struct inode *newdir,
			struct dentry *newent, unsigned int flags)
{
	struct fuse_conn *fc = get_fuse_conn(olddir);
	int err;

	if (fuse_is_bad(olddir))
		return -EIO;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	if (flags) {
#ifdef CONFIG_FUSE_BPF
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(olddir, struct fuse_rename2_in,
						fuse_rename2_initialize, fuse_rename2_backing,
						fuse_rename2_finalize,
						olddir, oldent, newdir, newent, flags);
		if (fer.ret)
			return PTR_ERR(fer.result);
#endif

		/* TODO: how should this go with bpfs involved? */
		if (fc->no_rename2 || fc->minor < 23)
			return -EINVAL;

		err = fuse_rename_common(olddir, oldent, newdir, newent, flags,
					 FUSE_RENAME2,
					 sizeof(struct fuse_rename2_in));
		if (err == -ENOSYS) {
			fc->no_rename2 = 1;
			err = -EINVAL;
		}
	} else {
#ifdef CONFIG_FUSE_BPF
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(olddir, struct fuse_rename_in,
						fuse_rename_initialize, fuse_rename_backing,
						fuse_rename_finalize,
						olddir, oldent, newdir, newent);
		if (fer.ret)
			return PTR_ERR(fer.result);
#endif

		err = fuse_rename_common(olddir, oldent, newdir, newent, 0,
					 FUSE_RENAME,
					 sizeof(struct fuse_rename_in));
	}

	return err;
}

static int fuse_link(struct dentry *entry, struct inode *newdir,
		     struct dentry *newent)
{
	int err;
	struct fuse_link_in inarg;
	struct inode *inode = d_inode(entry);
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_link_in, fuse_link_initialize,
			       fuse_link_backing, fuse_link_finalize, entry,
			       newdir, newent);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	memset(&inarg, 0, sizeof(inarg));
	inarg.oldnodeid = get_node_id(inode);
	args.opcode = FUSE_LINK;
	args.in_numargs = 2;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.in_args[1].size = newent->d_name.len + 1;
	args.in_args[1].value = newent->d_name.name;
	err = create_new_entry(fm, &args, newdir, newent, inode->i_mode);
	if (!err)
		fuse_update_ctime_in_cache(inode);
	else if (err == -EINTR)
		fuse_invalidate_attr(inode);

	if (err == -ENOSYS)
		err = -EPERM;
	return err;
}

void fuse_fillattr(struct inode *inode, struct fuse_attr *attr,
			  struct kstat *stat)
{
	unsigned int blkbits;
	struct fuse_conn *fc = get_fuse_conn(inode);

	stat->dev = inode->i_sb->s_dev;
	stat->ino = attr->ino;
	stat->mode = (inode->i_mode & S_IFMT) | (attr->mode & 07777);
	stat->nlink = attr->nlink;
	stat->uid = make_kuid(fc->user_ns, attr->uid);
	stat->gid = make_kgid(fc->user_ns, attr->gid);
	stat->rdev = inode->i_rdev;
	stat->atime.tv_sec = attr->atime;
	stat->atime.tv_nsec = attr->atimensec;
	stat->mtime.tv_sec = attr->mtime;
	stat->mtime.tv_nsec = attr->mtimensec;
	stat->ctime.tv_sec = attr->ctime;
	stat->ctime.tv_nsec = attr->ctimensec;
	stat->size = attr->size;
	stat->blocks = attr->blocks;

	if (attr->blksize != 0)
		blkbits = ilog2(attr->blksize);
	else
		blkbits = inode->i_sb->s_blocksize_bits;

	stat->blksize = 1 << blkbits;
}

static int fuse_do_getattr(struct inode *inode, struct kstat *stat,
			   struct file *file)
{
	int err;
	struct fuse_getattr_in inarg;
	struct fuse_attr_out outarg;
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	u64 attr_version;

	attr_version = fuse_get_attr_version(fm->fc);

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	/* Directories have separate file-handle space */
	if (file && S_ISREG(inode->i_mode)) {
		struct fuse_file *ff = file->private_data;

		inarg.getattr_flags |= FUSE_GETATTR_FH;
		inarg.fh = ff->fh;
	}
	args.opcode = FUSE_GETATTR;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;
	err = fuse_simple_request(fm, &args);
	if (!err)
		err = finalize_attr(inode, &outarg, attr_version, stat);
	return err;
}

static int fuse_update_get_attr(struct inode *inode, struct file *file,
				const struct path *path,
				struct kstat *stat, u32 request_mask,
				unsigned int flags)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	int err = 0;
	bool sync;
	u32 inval_mask = READ_ONCE(fi->inval_mask);
	u32 cache_mask = fuse_get_cache_mask(inode);

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_getattr_io,
			       fuse_getattr_initialize,	fuse_getattr_backing,
			       fuse_getattr_finalize,
			       path->dentry, stat, request_mask, flags);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	if (flags & AT_STATX_FORCE_SYNC)
		sync = true;
	else if (flags & AT_STATX_DONT_SYNC)
		sync = false;
	else if (request_mask & inval_mask & ~cache_mask)
		sync = true;
	else
		sync = time_before64(fi->i_time, get_jiffies_64());

	if (sync) {
		forget_all_cached_acls(inode);
		err = fuse_do_getattr(inode, stat, file);
	} else if (stat) {
		generic_fillattr(&init_user_ns, inode, stat);
		stat->mode = fi->orig_i_mode;
		stat->ino = fi->orig_ino;
	}

	return err;
}

int fuse_update_attributes(struct inode *inode, struct file *file, u32 mask)
{
	/* Do *not* need to get atime for internal purposes */
	return fuse_update_get_attr(inode, file, &file->f_path, NULL,
				    mask & ~STATX_ATIME, 0);
}

int fuse_reverse_inval_entry(struct fuse_conn *fc, u64 parent_nodeid,
			     u64 child_nodeid, struct qstr *name, u32 flags)
{
	int err = -ENOTDIR;
	struct inode *parent;
	struct dentry *dir;
	struct dentry *entry;

	parent = fuse_ilookup(fc, parent_nodeid, NULL);
	if (!parent)
		return -ENOENT;

	inode_lock_nested(parent, I_MUTEX_PARENT);
	if (!S_ISDIR(parent->i_mode))
		goto unlock;

	err = -ENOENT;
	dir = d_find_alias(parent);
	if (!dir)
		goto unlock;

	name->hash = full_name_hash(dir, name->name, name->len);
	entry = d_lookup(dir, name);
	dput(dir);
	if (!entry)
		goto unlock;

	fuse_dir_changed(parent);
	if (!(flags & FUSE_EXPIRE_ONLY))
		d_invalidate(entry);
	fuse_invalidate_entry_cache(entry);

	if (child_nodeid != 0 && d_really_is_positive(entry)) {
		inode_lock(d_inode(entry));
		if (get_node_id(d_inode(entry)) != child_nodeid) {
			err = -ENOENT;
			goto badentry;
		}
		if (d_mountpoint(entry)) {
			err = -EBUSY;
			goto badentry;
		}
		if (d_is_dir(entry)) {
			shrink_dcache_parent(entry);
			if (!simple_empty(entry)) {
				err = -ENOTEMPTY;
				goto badentry;
			}
			d_inode(entry)->i_flags |= S_DEAD;
		}
		dont_mount(entry);
		clear_nlink(d_inode(entry));
		err = 0;
 badentry:
		inode_unlock(d_inode(entry));
		if (!err)
			d_delete(entry);
	} else {
		err = 0;
	}
	dput(entry);

 unlock:
	inode_unlock(parent);
	iput(parent);
	return err;
}

/*
 * Calling into a user-controlled filesystem gives the filesystem
 * daemon ptrace-like capabilities over the current process.  This
 * means, that the filesystem daemon is able to record the exact
 * filesystem operations performed, and can also control the behavior
 * of the requester process in otherwise impossible ways.  For example
 * it can delay the operation for arbitrary length of time allowing
 * DoS against the requester.
 *
 * For this reason only those processes can call into the filesystem,
 * for which the owner of the mount has ptrace privilege.  This
 * excludes processes started by other users, suid or sgid processes.
 */
int fuse_allow_current_process(struct fuse_conn *fc)
{
	const struct cred *cred;

	if (allow_sys_admin_access && capable(CAP_SYS_ADMIN))
		return 1;

	if (fc->allow_other)
		return current_in_userns(fc->user_ns);

	cred = current_cred();
	if (uid_eq(cred->euid, fc->user_id) &&
	    uid_eq(cred->suid, fc->user_id) &&
	    uid_eq(cred->uid,  fc->user_id) &&
	    gid_eq(cred->egid, fc->group_id) &&
	    gid_eq(cred->sgid, fc->group_id) &&
	    gid_eq(cred->gid,  fc->group_id))
		return 1;

	return 0;
}

static int fuse_access(struct inode *inode, int mask)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	struct fuse_access_in inarg;
	int err;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_access_in,
			       fuse_access_initialize, fuse_access_backing,
			       fuse_access_finalize, inode, mask);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	BUG_ON(mask & MAY_NOT_BLOCK);

	if (fm->fc->no_access)
		return 0;

	memset(&inarg, 0, sizeof(inarg));
	inarg.mask = mask & (MAY_READ | MAY_WRITE | MAY_EXEC);
	args.opcode = FUSE_ACCESS;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	err = fuse_simple_request(fm, &args);
	if (err == -ENOSYS) {
		fm->fc->no_access = 1;
		err = 0;
	}
	return err;
}

static int fuse_perm_getattr(struct inode *inode, int mask)
{
	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;

	forget_all_cached_acls(inode);
	return fuse_do_getattr(inode, NULL, NULL);
}

/*
 * Check permission.  The two basic access models of FUSE are:
 *
 * 1) Local access checking ('default_permissions' mount option) based
 * on file mode.  This is the plain old disk filesystem permission
 * modell.
 *
 * 2) "Remote" access checking, where server is responsible for
 * checking permission in each inode operation.  An exception to this
 * is if ->permission() was invoked from sys_access() in which case an
 * access request is sent.  Execute permission is still checked
 * locally based on file mode.
 */
static int fuse_permission(struct user_namespace *mnt_userns,
			   struct inode *inode, int mask)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	bool refreshed = false;
	int err = 0;
	struct fuse_inode *fi = get_fuse_inode(inode);
#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;
#endif

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc))
		return -EACCES;

#ifdef CONFIG_FUSE_BPF
	fer = fuse_bpf_backing(inode, struct fuse_access_in,
			       fuse_access_initialize, fuse_access_backing,
			       fuse_access_finalize, inode, mask);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	/*
	 * If attributes are needed, refresh them before proceeding
	 */
	if (fc->default_permissions ||
	    ((mask & MAY_EXEC) && S_ISREG(inode->i_mode))) {
		u32 perm_mask = STATX_MODE | STATX_UID | STATX_GID;

		if (perm_mask & READ_ONCE(fi->inval_mask) ||
		    time_before64(fi->i_time, get_jiffies_64())) {
			refreshed = true;

			err = fuse_perm_getattr(inode, mask);
			if (err)
				return err;
		}
	}

	if (fc->default_permissions) {
		err = generic_permission(&init_user_ns, inode, mask);

		/* If permission is denied, try to refresh file
		   attributes.  This is also needed, because the root
		   node will at first have no permissions */
		if (err == -EACCES && !refreshed) {
			err = fuse_perm_getattr(inode, mask);
			if (!err)
				err = generic_permission(&init_user_ns,
							 inode, mask);
		}

		/* Note: the opposite of the above test does not
		   exist.  So if permissions are revoked this won't be
		   noticed immediately, only after the attribute
		   timeout has expired */
	} else if (mask & (MAY_ACCESS | MAY_CHDIR)) {
		err = fuse_access(inode, mask);
	} else if ((mask & MAY_EXEC) && S_ISREG(inode->i_mode)) {
		if (!(inode->i_mode & S_IXUGO)) {
			if (refreshed)
				return -EACCES;

			err = fuse_perm_getattr(inode, mask);
			if (!err && !(inode->i_mode & S_IXUGO))
				return -EACCES;
		}
	}
	return err;
}

static int fuse_readlink_page(struct inode *inode, struct page *page)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_page_desc desc = { .length = PAGE_SIZE - 1 };
	struct fuse_args_pages ap = {
		.num_pages = 1,
		.pages = &page,
		.descs = &desc,
	};
	char *link;
	ssize_t res;

	ap.args.opcode = FUSE_READLINK;
	ap.args.nodeid = get_node_id(inode);
	ap.args.out_pages = true;
	ap.args.out_argvar = true;
	ap.args.page_zeroing = true;
	ap.args.out_numargs = 1;
	ap.args.out_args[0].size = desc.length;
	res = fuse_simple_request(fm, &ap.args);

	fuse_invalidate_atime(inode);

	if (res < 0)
		return res;

	if (WARN_ON(res >= PAGE_SIZE))
		return -EIO;

	link = page_address(page);
	link[res] = '\0';

	return 0;
}

static const char *fuse_get_link(struct dentry *dentry, struct inode *inode,
				 struct delayed_call *callback)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct page *page;
	int err;

	err = -EIO;
	if (fuse_is_bad(inode))
		goto out_err;

#ifdef CONFIG_FUSE_BPF
	{
		struct fuse_err_ret fer;
		const char *out = NULL;

		fer = fuse_bpf_backing(inode, struct fuse_dummy_io,
				       fuse_get_link_initialize,
				       fuse_get_link_backing,
				       fuse_get_link_finalize,
				       inode, dentry, callback, &out);
		if (fer.ret)
			return fer.result ?: out;
	}
#endif

	if (fc->cache_symlinks)
		return page_get_link_raw(dentry, inode, callback);

	err = -ECHILD;
	if (!dentry)
		goto out_err;

	page = alloc_page(GFP_KERNEL);
	err = -ENOMEM;
	if (!page)
		goto out_err;

	err = fuse_readlink_page(inode, page);
	if (err) {
		__free_page(page);
		goto out_err;
	}

	set_delayed_call(callback, page_put_link, page);

	return page_address(page);

out_err:
	return ERR_PTR(err);
}

static int fuse_dir_open(struct inode *inode, struct file *file)
{
	return fuse_open_common(inode, file, true);
}

static int fuse_dir_release(struct inode *inode, struct file *file)
{
	fuse_release_common(file, true);
	return 0;
}

static int fuse_dir_fsync(struct file *file, loff_t start, loff_t end,
			  int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct fuse_conn *fc = get_fuse_conn(inode);
	int err;

	if (fuse_is_bad(inode))
		return -EIO;

#ifdef CONFIG_FUSE_BPF
	{
		struct fuse_err_ret fer;

		fer = fuse_bpf_backing(inode, struct fuse_fsync_in,
				fuse_dir_fsync_initialize, fuse_fsync_backing,
				fuse_fsync_finalize,
				file, start, end, datasync);
		if (fer.ret)
			return PTR_ERR(fer.result);
	}
#endif

	if (fc->no_fsyncdir)
		return 0;

	inode_lock(inode);
	err = fuse_fsync_common(file, start, end, datasync, FUSE_FSYNCDIR);
	if (err == -ENOSYS) {
		fc->no_fsyncdir = 1;
		err = 0;
	}
	inode_unlock(inode);

	return err;
}

static long fuse_dir_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct fuse_conn *fc = get_fuse_conn(file->f_mapping->host);

	/* FUSE_IOCTL_DIR only supported for API version >= 7.18 */
	if (fc->minor < 18)
		return -ENOTTY;

	return fuse_ioctl_common(file, cmd, arg, FUSE_IOCTL_DIR);
}

static long fuse_dir_compat_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct fuse_conn *fc = get_fuse_conn(file->f_mapping->host);

	if (fc->minor < 18)
		return -ENOTTY;

	return fuse_ioctl_common(file, cmd, arg,
				 FUSE_IOCTL_COMPAT | FUSE_IOCTL_DIR);
}

/*
 * Prevent concurrent writepages on inode
 *
 * This is done by adding a negative bias to the inode write counter
 * and waiting for all pending writes to finish.
 */
void fuse_set_nowrite(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	BUG_ON(!inode_is_locked(inode));

	spin_lock(&fi->lock);
	BUG_ON(fi->writectr < 0);
	fi->writectr += FUSE_NOWRITE;
	spin_unlock(&fi->lock);
	wait_event(fi->page_waitq, fi->writectr == FUSE_NOWRITE);
}

/*
 * Allow writepages on inode
 *
 * Remove the bias from the writecounter and send any queued
 * writepages.
 */
static void __fuse_release_nowrite(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	BUG_ON(fi->writectr != FUSE_NOWRITE);
	fi->writectr = 0;
	fuse_flush_writepages(inode);
}

void fuse_release_nowrite(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	spin_lock(&fi->lock);
	__fuse_release_nowrite(inode);
	spin_unlock(&fi->lock);
}

static void fuse_setattr_fill(struct fuse_conn *fc, struct fuse_args *args,
			      struct inode *inode,
			      struct fuse_setattr_in *inarg_p,
			      struct fuse_attr_out *outarg_p)
{
	args->opcode = FUSE_SETATTR;
	args->nodeid = get_node_id(inode);
	args->in_numargs = 1;
	args->in_args[0].size = sizeof(*inarg_p);
	args->in_args[0].value = inarg_p;
	args->out_numargs = 1;
	args->out_args[0].size = sizeof(*outarg_p);
	args->out_args[0].value = outarg_p;
}

/*
 * Flush inode->i_mtime to the server
 */
int fuse_flush_times(struct inode *inode, struct fuse_file *ff)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	FUSE_ARGS(args);
	struct fuse_setattr_in inarg;
	struct fuse_attr_out outarg;

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));

	inarg.valid = FATTR_MTIME;
	inarg.mtime = inode->i_mtime.tv_sec;
	inarg.mtimensec = inode->i_mtime.tv_nsec;
	if (fm->fc->minor >= 23) {
		inarg.valid |= FATTR_CTIME;
		inarg.ctime = inode->i_ctime.tv_sec;
		inarg.ctimensec = inode->i_ctime.tv_nsec;
	}
	if (ff) {
		inarg.valid |= FATTR_FH;
		inarg.fh = ff->fh;
	}
	fuse_setattr_fill(fm->fc, &args, inode, &inarg, &outarg);

	return fuse_simple_request(fm, &args);
}

/*
 * Set attributes, and at the same time refresh them.
 *
 * Truncation is slightly complicated, because the 'truncate' request
 * may fail, in which case we don't want to touch the mapping.
 * vmtruncate() doesn't allow for this case, so do the rlimit checking
 * and the actual truncation by hand.
 */
int fuse_do_setattr(struct dentry *dentry, struct iattr *attr,
		    struct file *file)
{
	struct inode *inode = d_inode(dentry);
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_conn *fc = fm->fc;
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct address_space *mapping = inode->i_mapping;
	FUSE_ARGS(args);
	struct fuse_setattr_in inarg;
	struct fuse_attr_out outarg;
	bool is_truncate = false;
	bool is_wb = fc->writeback_cache && S_ISREG(inode->i_mode);
	loff_t oldsize;
	int err;
	bool trust_local_cmtime = is_wb;
	bool fault_blocked = false;

#ifdef CONFIG_FUSE_BPF
	struct fuse_err_ret fer;

	fer = fuse_bpf_backing(inode, struct fuse_setattr_io,
			       fuse_setattr_initialize, fuse_setattr_backing,
			       fuse_setattr_finalize, dentry, attr, file);
	if (fer.ret)
		return PTR_ERR(fer.result);
#endif

	if (!fc->default_permissions)
		attr->ia_valid |= ATTR_FORCE;

	err = setattr_prepare(&init_user_ns, dentry, attr);
	if (err)
		return err;

	if (attr->ia_valid & ATTR_SIZE) {
		if (WARN_ON(!S_ISREG(inode->i_mode)))
			return -EIO;
		is_truncate = true;
	}

	if (FUSE_IS_DAX(inode) && is_truncate) {
		filemap_invalidate_lock(mapping);
		fault_blocked = true;
		err = fuse_dax_break_layouts(inode, 0, -1);
		if (err) {
			filemap_invalidate_unlock(mapping);
			return err;
		}
	}

	if (attr->ia_valid & ATTR_OPEN) {
		/* This is coming from open(..., ... | O_TRUNC); */
		WARN_ON(!(attr->ia_valid & ATTR_SIZE));
		WARN_ON(attr->ia_size != 0);
		if (fc->atomic_o_trunc) {
			/*
			 * No need to send request to userspace, since actual
			 * truncation has already been done by OPEN.  But still
			 * need to truncate page cache.
			 */
			i_size_write(inode, 0);
			truncate_pagecache(inode, 0);
			goto out;
		}
		file = NULL;
	}

	/* Flush dirty data/metadata before non-truncate SETATTR */
	if (is_wb &&
	    attr->ia_valid &
			(ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_MTIME_SET |
			 ATTR_TIMES_SET)) {
		err = write_inode_now(inode, true);
		if (err)
			return err;

		fuse_set_nowrite(inode);
		fuse_release_nowrite(inode);
	}

	if (is_truncate) {
		fuse_set_nowrite(inode);
		set_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);
		if (trust_local_cmtime && attr->ia_size != inode->i_size)
			attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
	}

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	iattr_to_fattr(fc, attr, &inarg, trust_local_cmtime);
	if (file) {
		struct fuse_file *ff = file->private_data;
		inarg.valid |= FATTR_FH;
		inarg.fh = ff->fh;
	}

	/* Kill suid/sgid for non-directory chown unconditionally */
	if (fc->handle_killpriv_v2 && !S_ISDIR(inode->i_mode) &&
	    attr->ia_valid & (ATTR_UID | ATTR_GID))
		inarg.valid |= FATTR_KILL_SUIDGID;

	if (attr->ia_valid & ATTR_SIZE) {
		/* For mandatory locking in truncate */
		inarg.valid |= FATTR_LOCKOWNER;
		inarg.lock_owner = fuse_lock_owner_id(fc, current->files);

		/* Kill suid/sgid for truncate only if no CAP_FSETID */
		if (fc->handle_killpriv_v2 && !capable(CAP_FSETID))
			inarg.valid |= FATTR_KILL_SUIDGID;
	}
	fuse_setattr_fill(fc, &args, inode, &inarg, &outarg);
	err = fuse_simple_request(fm, &args);
	if (err) {
		if (err == -EINTR)
			fuse_invalidate_attr(inode);
		goto error;
	}

	if (fuse_invalid_attr(&outarg.attr) ||
	    inode_wrong_type(inode, outarg.attr.mode)) {
		fuse_make_bad(inode);
		err = -EIO;
		goto error;
	}

	spin_lock(&fi->lock);
	/* the kernel maintains i_mtime locally */
	if (trust_local_cmtime) {
		if (attr->ia_valid & ATTR_MTIME)
			inode->i_mtime = attr->ia_mtime;
		if (attr->ia_valid & ATTR_CTIME)
			inode->i_ctime = attr->ia_ctime;
		/* FIXME: clear I_DIRTY_SYNC? */
	}

	fuse_change_attributes_common(inode, &outarg.attr,
				      attr_timeout(&outarg),
				      fuse_get_cache_mask(inode));
	oldsize = inode->i_size;
	/* see the comment in fuse_change_attributes() */
	if (!is_wb || is_truncate)
		i_size_write(inode, outarg.attr.size);

	if (is_truncate) {
		/* NOTE: this may release/reacquire fi->lock */
		__fuse_release_nowrite(inode);
	}
	spin_unlock(&fi->lock);

	/*
	 * Only call invalidate_inode_pages2() after removing
	 * FUSE_NOWRITE, otherwise fuse_launder_folio() would deadlock.
	 */
	if ((is_truncate || !is_wb) &&
	    S_ISREG(inode->i_mode) && oldsize != outarg.attr.size) {
		truncate_pagecache(inode, outarg.attr.size);
		invalidate_inode_pages2(mapping);
	}

	clear_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);
out:
	if (fault_blocked)
		filemap_invalidate_unlock(mapping);

	return 0;

error:
	if (is_truncate)
		fuse_release_nowrite(inode);

	clear_bit(FUSE_I_SIZE_UNSTABLE, &fi->state);

	if (fault_blocked)
		filemap_invalidate_unlock(mapping);
	return err;
}

static int fuse_setattr(struct user_namespace *mnt_userns, struct dentry *entry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(entry);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct file *file = (attr->ia_valid & ATTR_FILE) ? attr->ia_file : NULL;
	int ret;

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(get_fuse_conn(inode)))
		return -EACCES;

	if (attr->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID)) {
		attr->ia_valid &= ~(ATTR_KILL_SUID | ATTR_KILL_SGID |
				    ATTR_MODE);

		/*
		 * The only sane way to reliably kill suid/sgid is to do it in
		 * the userspace filesystem
		 *
		 * This should be done on write(), truncate() and chown().
		 */
		if (!fc->handle_killpriv && !fc->handle_killpriv_v2) {
#ifdef CONFIG_FUSE_BPF
			struct fuse_err_ret fer;

			/*
			 * ia_mode calculation may have used stale i_mode.
			 * Refresh and recalculate.
			 */
			fer = fuse_bpf_backing(inode, struct fuse_getattr_io,
					       fuse_getattr_initialize,	fuse_getattr_backing,
					       fuse_getattr_finalize,
					       entry, NULL, 0, 0);
			if (fer.ret)
				ret = PTR_ERR(fer.result);
			else
#endif
				ret = fuse_do_getattr(inode, NULL, file);
			if (ret)
				return ret;

			attr->ia_mode = inode->i_mode;
			if (inode->i_mode & S_ISUID) {
				attr->ia_valid |= ATTR_MODE;
				attr->ia_mode &= ~S_ISUID;
			}
			if ((inode->i_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
				attr->ia_valid |= ATTR_MODE;
				attr->ia_mode &= ~S_ISGID;
			}
		}
	}
	if (!attr->ia_valid)
		return 0;

	ret = fuse_do_setattr(entry, attr, file);
	if (!ret) {
		/*
		 * If filesystem supports acls it may have updated acl xattrs in
		 * the filesystem, so forget cached acls for the inode.
		 */
		if (fc->posix_acl)
			forget_all_cached_acls(inode);

		/* Directory mode changed, may need to revalidate access */
		if (d_is_dir(entry) && (attr->ia_valid & ATTR_MODE))
			fuse_invalidate_entry_cache(entry);
	}
	return ret;
}

static int fuse_getattr(struct user_namespace *mnt_userns,
			const struct path *path, struct kstat *stat,
			u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct fuse_conn *fc = get_fuse_conn(inode);

	if (fuse_is_bad(inode))
		return -EIO;

	if (!fuse_allow_current_process(fc)) {
		if (!request_mask) {
			/*
			 * If user explicitly requested *nothing* then don't
			 * error out, but return st_dev only.
			 */
			stat->result_mask = 0;
			stat->dev = inode->i_sb->s_dev;
			return 0;
		}
		return -EACCES;
	}

	return fuse_update_get_attr(inode, NULL, path, stat, request_mask,
				    flags);
}

static const struct inode_operations fuse_dir_inode_operations = {
	.lookup		= fuse_lookup,
	.mkdir		= fuse_mkdir,
	.symlink	= fuse_symlink,
	.unlink		= fuse_unlink,
	.rmdir		= fuse_rmdir,
	.rename		= fuse_rename2,
	.link		= fuse_link,
	.setattr	= fuse_setattr,
	.create		= fuse_create,
	.atomic_open	= fuse_atomic_open,
	.tmpfile	= fuse_tmpfile,
	.mknod		= fuse_mknod,
	.permission	= fuse_permission,
	.getattr	= fuse_getattr,
	.listxattr	= fuse_listxattr,
	.get_acl	= fuse_get_acl,
	.set_acl	= fuse_set_acl,
	.fileattr_get	= fuse_fileattr_get,
	.fileattr_set	= fuse_fileattr_set,
};

static const struct file_operations fuse_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= fuse_readdir,
	.open		= fuse_dir_open,
	.release	= fuse_dir_release,
	.fsync		= fuse_dir_fsync,
	.unlocked_ioctl	= fuse_dir_ioctl,
	.compat_ioctl	= fuse_dir_compat_ioctl,
};

static const struct inode_operations fuse_common_inode_operations = {
	.setattr	= fuse_setattr,
	.permission	= fuse_permission,
	.getattr	= fuse_getattr,
	.listxattr	= fuse_listxattr,
	.get_acl	= fuse_get_acl,
	.set_acl	= fuse_set_acl,
	.fileattr_get	= fuse_fileattr_get,
	.fileattr_set	= fuse_fileattr_set,
};

static const struct inode_operations fuse_symlink_inode_operations = {
	.setattr	= fuse_setattr,
	.get_link	= fuse_get_link,
	.getattr	= fuse_getattr,
	.listxattr	= fuse_listxattr,
};

void fuse_init_common(struct inode *inode)
{
	inode->i_op = &fuse_common_inode_operations;
}

void fuse_init_dir(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);

	inode->i_op = &fuse_dir_inode_operations;
	inode->i_fop = &fuse_dir_operations;

	spin_lock_init(&fi->rdc.lock);
	fi->rdc.cached = false;
	fi->rdc.size = 0;
	fi->rdc.pos = 0;
	fi->rdc.version = 0;
}

static int fuse_symlink_read_folio(struct file *null, struct folio *folio)
{
	int err = fuse_readlink_page(folio->mapping->host, &folio->page);

	if (!err)
		folio_mark_uptodate(folio);

	folio_unlock(folio);

	return err;
}

static const struct address_space_operations fuse_symlink_aops = {
	.read_folio	= fuse_symlink_read_folio,
};

void fuse_init_symlink(struct inode *inode)
{
	inode->i_op = &fuse_symlink_inode_operations;
	inode->i_data.a_ops = &fuse_symlink_aops;
	inode_nohighmem(inode);
}
