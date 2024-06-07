#include <unix_internal.h>
#include <filesystem.h>
#include <storage.h>

#define FS_KNOWN_SEALS  \
    (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)

sysreturn sysreturn_from_fs_status(fs_status s)
{
    switch (s) {
    case FS_STATUS_NOSPACE:
        return -ENOSPC;
    case FS_STATUS_IOERR:
        return -EIO;
    case FS_STATUS_NOENT:
        return -ENOENT;
    case FS_STATUS_EXIST:
        return -EEXIST;
    case FS_STATUS_INVAL:
        return -EINVAL;
    case FS_STATUS_NOTDIR:
        return -ENOTDIR;
    case FS_STATUS_ISDIR:
        return -EISDIR;
    case FS_STATUS_NOTEMPTY:
        return -ENOTEMPTY;
    case FS_STATUS_NOMEM:
        return -ENOMEM;
    case FS_STATUS_LINKLOOP:
        return -ELOOP;
    case FS_STATUS_NAMETOOLONG:
        return -ENAMETOOLONG;
    case FS_STATUS_XDEV:
        return -EXDEV;
    case FS_STATUS_FAULT:
        return -EFAULT;
    case FS_STATUS_READONLY:
        return -EROFS;
    default:
        return 0;
    }
}

sysreturn sysreturn_from_fs_status_value(status s)
{
    if (is_ok(s))
        return 0;
    u64 fss;
    sysreturn rv;

    /* block r/w errors won't include an fs status, so assume I/O error if none found */
    if (get_u64(s, sym(fsstatus), &fss))
        rv = sysreturn_from_fs_status(fss);
    else
        rv = -EIO;
    return rv;
}

void file_readahead(file f, u64 offset, u64 len)
{
    u64 ra_size = 0;
    switch (f->fadv) {
    case POSIX_FADV_NORMAL:
        ra_size = FILE_READAHEAD_DEFAULT;
        break;
    case POSIX_FADV_RANDOM: /* no read-ahead */
        break;
    case POSIX_FADV_SEQUENTIAL:
        ra_size = 2 * FILE_READAHEAD_DEFAULT;
        break;
    }
    if (ra_size > 0)
        pagecache_node_fetch_pages(fsfile_get_cachenode(f->fsf),
            irangel(offset + len, ra_size));
}

fs_status filesystem_chdir(process p, sstring path)
{
    process_lock(p);
    filesystem fs = p->cwd_fs;
    fs_status fss;
    tuple n;
    fss = filesystem_get_node(&fs, p->cwd, path, false, false, false, false, &n, 0);
    if (fss != FS_STATUS_OK)
        goto out;
    if (!is_dir(n)) {
        fss = FS_STATUS_NOENT;
    } else {
        if (fs != p->cwd_fs) {
            filesystem_release(p->cwd_fs);
            filesystem_reserve(fs);
            p->cwd_fs = fs;
        }
        p->cwd = fs->get_inode(fs, n);
        fss = FS_STATUS_OK;
    }
    filesystem_put_node(fs, n);
  out:
    process_unlock(p);
    return fss;
}

void filesystem_update_relatime(filesystem fs, tuple md)
{
    timestamp here = now(CLOCK_ID_REALTIME);
    timestamp atime = filesystem_get_atime(fs, md);
    boolean update;
    if (here > atime + seconds(24 * 60 * 60))
        update = true;
    else
        update = (atime <= filesystem_get_mtime(fs, md));
    if (update)
        filesystem_set_atime(fs, md, here);
}

closure_function(2, 2, void, fs_op_complete,
                 thread, t, file, f,
                 fsfile fsf, fs_status s)
{
    thread t = bound(t);
    sysreturn ret = sysreturn_from_fs_status(s);

    fdesc_put(&bound(f)->f);
    syscall_return(t, ret);     /* returns on kernel context */
    closure_finish();
}

static sysreturn symlink_internal(filesystem fs, inode cwd, sstring path,
        const char *target)
{
    sstring target_ss;
    if (!fault_in_user_string(target, &target_ss))
        return -EFAULT;
    return sysreturn_from_fs_status(filesystem_symlink(fs, cwd, path, target_ss));
}

sysreturn symlink(const char *target, const char *linkpath)
{
    sstring path_ss;
    if (!fault_in_user_string(linkpath, &path_ss))
        return -EFAULT;
    filesystem cwd_fs;
    inode cwd;
    process_get_cwd(current->p, &cwd_fs, &cwd);
    sysreturn rv = symlink_internal(cwd_fs, cwd, path_ss, target);
    filesystem_release(cwd_fs);
    return rv;
}

sysreturn symlinkat(const char *target, int dirfd, const char *linkpath)
{
    filesystem fs;
    sstring path_ss;
    inode cwd = resolve_dir(fs, dirfd, linkpath, path_ss);
    sysreturn rv = symlink_internal(fs, cwd, path_ss, target);
    filesystem_release(fs);
    return rv;
}

static sysreturn utime_internal(const char *filename, timestamp actime,
        timestamp modtime)
{
    tuple t;
    filesystem fs;
    inode cwd;
    sstring filename_ss;
    if (!fault_in_user_string(filename, &filename_ss))
        return -EFAULT;
    process_get_cwd(current->p, &fs, &cwd);
    filesystem cwd_fs = fs;
    fs_status fss = filesystem_get_node(&fs, cwd, filename_ss, false, false, false, false, &t, 0);
    sysreturn rv;
    if (fss != FS_STATUS_OK) {
        rv = sysreturn_from_fs_status(fss);
    } else {
        filesystem_set_atime(fs, t, actime);
        filesystem_set_mtime(fs, t, modtime);
        filesystem_put_node(fs, t);
        rv = 0;
    }
    filesystem_release(cwd_fs);
    return rv;
}

sysreturn utime(const char *filename, const struct utimbuf *times)
{
    context ctx;
    if (times) {
        ctx = get_current_context(current_cpu());
        if (!validate_user_memory(times, sizeof(struct utimbuf), false) || context_set_err(ctx))
            return -EFAULT;
    }
    timestamp atime = times ? seconds(times->actime) : now(CLOCK_ID_REALTIME);
    timestamp mtime = times ? seconds(times->modtime) : now(CLOCK_ID_REALTIME);
    if (times)
        context_clear_err(ctx);
    return utime_internal(filename, atime, mtime);
}

sysreturn utimes(const char *filename, const struct timeval times[2])
{
    context ctx;
    if (times) {
        ctx = get_current_context(current_cpu());
        if (!validate_user_memory(times, 2 * sizeof(struct timeval), false) || context_set_err(ctx))
            return -EFAULT;
    }
    /* Sub-second precision is not supported. */
    timestamp atime =
            times ? time_from_timeval(&times[0]) : now(CLOCK_ID_REALTIME);
    timestamp mtime =
            times ? time_from_timeval(&times[1]) : now(CLOCK_ID_REALTIME);
    if (times)
        context_clear_err(ctx);
    return utime_internal(filename, atime, mtime);
}

static boolean utimens_is_valid(const struct timespec *t)
{
    return (t->tv_nsec < BILLION) || (t->tv_nsec == UTIME_NOW) || (t->tv_nsec == UTIME_OMIT);
}

static timestamp time_from_utimens(const struct timespec *t)
{
    if (t->tv_nsec == UTIME_NOW)
        return now(CLOCK_ID_REALTIME);
    if (t->tv_nsec == UTIME_OMIT)
        return infinity;
    return time_from_timespec(t);
}

sysreturn utimensat(int dirfd, const char *filename, const struct timespec times[2], int flags)
{
    timestamp atime, mtime;
    if (times) {
        context ctx = get_current_context(current_cpu());
        if (!validate_user_memory(times, 2 * sizeof(struct timespec), false) ||
            context_set_err(ctx))
            return -EFAULT;
        if (!utimens_is_valid(&times[0]) || !utimens_is_valid(&times[1]))
            return -EINVAL;
        atime = time_from_utimens(&times[0]);
        mtime = time_from_utimens(&times[1]);
        context_clear_err(ctx);
    } else {
        atime = mtime = now(CLOCK_ID_REALTIME);
    }
    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    tuple t;
    filesystem fs, cwd_fs;
    sysreturn rv;
    if (filename) {
        sstring filename_ss;
        inode cwd = resolve_dir(fs, dirfd, filename, filename_ss);
        cwd_fs = fs;
        fs_status fss = filesystem_get_node(&fs, cwd, filename_ss, !!(flags & AT_SYMLINK_NOFOLLOW),
                                            false, false, false, &t, 0);
        rv = sysreturn_from_fs_status(fss);
        if (rv)
            filesystem_release(cwd_fs);
    } else {
        file f = resolve_fd(current->p, dirfd);
        switch (f->f.type) {
        case FDESC_TYPE_REGULAR:
        case FDESC_TYPE_DIRECTORY:
        case FDESC_TYPE_SYMLINK:
        case FDESC_TYPE_SOCKET:
            fs = f->fs;
            t = filesystem_get_meta(fs, f->n);
            rv = t ? 0 : -ENOENT;
            break;
        default:
            rv = -EACCES;
        }
        fdesc_put(&f->f);
    }
    if (rv == 0) {
        if (atime != infinity)
            filesystem_set_atime(fs, t, atime);
        if (mtime != infinity)
            filesystem_set_mtime(fs, t, mtime);
        if (filename) {
            filesystem_put_node(fs, t);
            filesystem_release(cwd_fs);
        } else {
            filesystem_put_meta(fs, t);
        }
    }
    return rv;
}

static sysreturn statfs_internal(filesystem fs, tuple t, struct statfs *buf)
{
    if (!fault_in_user_memory(buf, sizeof(struct statfs), true))
        return -EFAULT;
    runtime_memset((u8 *) buf, 0, sizeof(*buf));
    if (fs) {
        buf->f_bsize = fs_blocksize(fs);
        buf->f_blocks = fs_totalblocks(fs);
        buf->f_bfree = buf->f_bavail = fs_freeblocks(fs);
    } else {
        buf->f_bsize = PAGESIZE;
    }
    buf->f_frsize = buf->f_bsize;
    u64 id = u64_from_pointer(t);
    buf->f_fsid.val[0] = (int) id;
    buf->f_fsid.val[1] = (int) (id >> 32);
    buf->f_namelen = NAME_MAX;
    return set_syscall_return(current, 0);
}

sysreturn statfs(const char *path, struct statfs *buf)
{
    filesystem fs;
    inode cwd;
    process_get_cwd(current->p, &fs, &cwd);
    sstring path_ss;
    filesystem cwd_fs = fs;
    tuple t = 0;
    sysreturn rv;
    if (!fault_in_user_string(path, &path_ss)) {
        rv = -EFAULT;
        goto out;
    }
    fs_status fss = filesystem_get_node(&fs, cwd, path_ss, true, false, false, false, &t, 0);
    if (fss != FS_STATUS_OK) {
        rv = sysreturn_from_fs_status(fss);
    } else {
        rv = statfs_internal(fs, t, buf);
    }
  out:
    if (t)
        filesystem_put_node(fs, t);
    filesystem_release(cwd_fs);
    return rv;
}

sysreturn fstatfs(int fd, struct statfs *buf)
{
    fdesc desc = resolve_fd(current->p, fd);
    file f;
    switch (desc->type) {
    case FDESC_TYPE_REGULAR:
    case FDESC_TYPE_DIRECTORY:
    case FDESC_TYPE_SYMLINK:
        f = (file) desc;
        break;
    default:
        f = 0;
        break;
    }
    tuple t = 0;
    sysreturn rv;
    if (f)
        t = filesystem_get_meta(f->fs, f->n);
    rv = statfs_internal(f ? f->fs : 0, t, buf);
    fdesc_put(desc);
    if (t)
        filesystem_put_meta(f->fs, t);
    return rv;
}

sysreturn fallocate(int fd, int mode, long offset, long len)
{
    fdesc desc = resolve_fd(current->p, fd);
    sysreturn rv;
    if (desc->type != FDESC_TYPE_REGULAR) {
        switch (desc->type) {
        case FDESC_TYPE_PIPE:
        case FDESC_TYPE_STDIO:
            rv = -ESPIPE;
            break;
        default:
            rv = -ENODEV;
        }
        goto out;
    } else if (!fdesc_is_writable(desc)) {
        rv = -EBADF;
        goto out;
    }

    heap h = heap_locked(get_kernel_heaps());
    file f = (file) desc;
    switch (mode) {
    case 0:
    case FALLOC_FL_KEEP_SIZE:
        filesystem_alloc(f->fsf, offset, len,
                         mode == FALLOC_FL_KEEP_SIZE,
                         closure(h, fs_op_complete, current, f));
        break;
    case FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE:
        filesystem_dealloc(f->fsf, offset, len,
                           closure(h, fs_op_complete, current, f));
        break;
    default:
        rv = -EINVAL;
        goto out;
    }
    return thread_maybe_sleep_uninterruptible(current);
  out:
    fdesc_put(desc);
    return rv;
}

sysreturn fadvise64(int fd, s64 off, u64 len, int advice)
{
    fdesc desc = resolve_fd(current->p, fd);
    sysreturn rv;
    if (desc->type != FDESC_TYPE_REGULAR) {
        switch (desc->type) {
        case FDESC_TYPE_PIPE:
        case FDESC_TYPE_STDIO:
            rv = -ESPIPE;
            break;
        default:
            rv = -EBADF;
        }
        goto out;
    }
    file f = (file)desc;
    switch (advice) {
    case POSIX_FADV_NORMAL:
    case POSIX_FADV_RANDOM:
    case POSIX_FADV_SEQUENTIAL:
        f->fadv = advice;
        break;
    case POSIX_FADV_WILLNEED: {
        pagecache_node pn = fsfile_get_cachenode(f->fsf);
        range r = (len != 0) ? irangel(off, len) :
                irange(off, pagecache_get_node_length(pn));
        pagecache_node_fetch_pages(pn, r);
        break;
    }
    case POSIX_FADV_DONTNEED:
    case POSIX_FADV_NOREUSE:
        break;
    default:
        rv = -EINVAL;
        goto out;
    }
    rv = 0;
  out:
    fdesc_put(desc);
    return rv;
}

void file_release(file f)
{
    release_fdesc(&f->f);
    filesystem_release(f->fs);
    if (f->f.type == FDESC_TYPE_SPECIAL)
        spec_deallocate(f);
    else
        unix_cache_free(get_unix_heaps(), file, f);
}

/* file_path is treated as an absolute path for fsfile_open() and fsfile_open_or_create() */
fsfile fsfile_open(sstring file_path)
{
    tuple file;
    fsfile fsf;
    filesystem fs = get_root_fs();
    fs_status s = filesystem_get_node(&fs, fs->get_inode(fs, filesystem_getroot(fs)),
                                      file_path,
                                      true, false, false, false, &file, &fsf);
    if (s == FS_STATUS_OK) {
        filesystem_put_node(fs, file);
        return fsf;
    }
    return 0;
}

fsfile fsfile_open_or_create(sstring file_path, boolean truncate)
{
    tuple file;
    fsfile fsf;
    filesystem fs = get_root_fs();
    tuple root = filesystem_getroot(fs);
    char *separator = runtime_strrchr(file_path, '/');
    fs_status s;
    if (separator > file_path.ptr) {
        s = filesystem_mkdirpath(fs, 0, isstring(file_path.ptr, separator - file_path.ptr), true);
        if ((s != FS_STATUS_OK) && (s != FS_STATUS_EXIST))
            return 0;
    }
    s = filesystem_get_node(&fs, fs->get_inode(fs, root), file_path, true, true, false, truncate,
                            &file, &fsf);
    if (s == FS_STATUS_OK) {
        filesystem_put_node(fs, file);
        return fsf;
    }
    return 0;
}

/* Can be used for files in the root filesystem only. */
fs_status fsfile_truncate(fsfile f, u64 len)
{
    return (filesystem_truncate(get_root_fs(), f, len));
}

closure_function(2, 1, boolean, fsfile_seal_vmap_handler,
                 pagecache_node, pn, boolean *, writable,
                 vmap vm)
{
    if ((vm->cache_node == bound(pn)) && (vm->allowed_flags & VMAP_FLAG_WRITABLE)) {
        *bound(writable) = true;
        return false;
    }
    return true;
}

sysreturn fsfile_add_seals(fsfile f, u64 seals)
{
    if (seals & ~FS_KNOWN_SEALS)
        return -EINVAL;
    filesystem fs = f->fs;
    if (!fs->set_seals)
        return -EINVAL;
    filesystem_lock(fs);
    u64 current_seals;
    fs_status fss = fs->get_seals(fs, f, &current_seals);
    sysreturn rv;
    if (fss == FS_STATUS_OK) {
        if (current_seals & F_SEAL_SEAL) {
            rv = -EPERM;
            goto out;
        }
        if (seals & F_SEAL_WRITE) {
            pagecache_node pn = fsfile_get_cachenode(f);
            boolean writable_maps = false;
            vmap_iterator(current->p, stack_closure(fsfile_seal_vmap_handler, pn, &writable_maps));
            if (writable_maps) {
                rv = -EBUSY;
                goto out;
            }
        }
        fss = fs->set_seals(fs, f, current_seals | seals);
    }
    rv = sysreturn_from_fs_status(fss);
  out:
    filesystem_unlock(fs);
    return rv;
}

sysreturn fsfile_get_seals(fsfile f, u64 *seals)
{
    filesystem fs = f->fs;
    if (!fs->get_seals)
        return -EINVAL;
    return sysreturn_from_fs_status(fs->get_seals(fs, f, seals));
}

notify_entry fs_watch(heap h, tuple n, u64 eventmask, event_handler eh, notify_set *s)
{
    tuple watches = get_tuple(n, sym(watches));
    notify_set ns;
    if (!watches) {
        ns = allocate_notify_set(h);
        if (ns == INVALID_ADDRESS)
            return 0;
        watches = allocate_tuple();
        set(watches, sym(no_encode), null_value);
        set(watches, sym(ns), ns);
        set(n, sym(watches), watches);
    } else {
        ns = get(watches, sym(ns));
    }
    notify_entry ne = notify_add(ns, eventmask, eh);
    if (ne != INVALID_ADDRESS) {
        *s = ns;
        return ne;
    }
    return 0;
}

static void fs_notify_internal(tuple md, u64 event, symbol name, u32 cookie)
{
    tuple watches = get_tuple(md, sym(watches));
    if (watches) {
        struct inotify_evdata evdata = {
            .name = name ? symbol_string(name) : 0,
            .cookie = cookie,
        };
        notify_dispatch_with_arg(get(watches, sym(ns)), event, &evdata);
    }
}

void fs_notify_event(tuple n, u64 event)
{
    if (is_dir(n))
        event |= IN_ISDIR;
    fs_notify_internal(n, event, 0, 0);
    tuple parent = get_tuple(n, sym_this(".."));
    if (parent != n)
        fs_notify_internal(parent, event, tuple_get_symbol(children(parent), n), 0);
}

void fs_notify_create(tuple t, tuple parent, symbol name)
{
    u64 event = IN_CREATE;
    if (is_dir(t))
        event |= IN_ISDIR;
    fs_notify_internal(parent, event, name, 0);
}

void fs_notify_move(tuple t, tuple old_parent, symbol old_name, tuple new_parent, symbol new_name)
{
    u64 flags = is_dir(t) ? IN_ISDIR : 0;
    fs_notify_internal(t, IN_MOVE_SELF | flags, 0, 0);
    u32 cookie = random_u64();
    fs_notify_internal(old_parent, IN_MOVED_FROM | flags, old_name, cookie);
    fs_notify_internal(new_parent, IN_MOVED_TO | flags, new_name, cookie);
}

void fs_notify_delete(tuple t, tuple parent, symbol name)
{
    u64 flags = is_dir(t) ? IN_ISDIR : 0;
    fs_notify_internal(t, IN_DELETE_SELF | flags, 0, 0);
    fs_notify_internal(parent, IN_DELETE | flags, name, 0);
}

void fs_notify_modify(tuple t)
{
    fs_notify_event(t, IN_MODIFY);
}

void fs_notify_release(tuple t, boolean unmounted)
{
    tuple watches = get_tuple(t, sym(watches));
    if (watches) {
        notify_set ns = get(watches, sym(ns));
        if (unmounted)
            notify_dispatch_with_arg(ns, IN_UNMOUNT, 0);
        deallocate_notify_set(ns);
        deallocate_value(watches);
        set(t, sym(watches), 0);
    }
}

boolean fs_file_is_busy(filesystem fs, tuple md)
{
    return (get_tuple(md, sym(watches)) != 0);
}
