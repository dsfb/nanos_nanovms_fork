#include "unix_internal.h"

#define AIO_RING_MAGIC  0xa10a10a1

#define AIO_KNOWN_FLAGS IOCB_FLAG_RESFD

#define AIO_RESFD_INVALID   -1U

#define aio_lock(aio)   spin_lock(&(aio)->lock)
#define aio_unlock(aio) spin_unlock(&(aio)->lock)

typedef struct aio_ring {
    unsigned int id;
    unsigned int nr;
    unsigned int head;
    unsigned int tail;
    unsigned int magic;
    unsigned int compat_features;
    unsigned int incompat_features;
    unsigned int header_length;
    struct io_event events[0];
} *aio_ring;

struct aio {
    struct list elem;  /* must be first */
    heap vh;
    kernel_heaps kh;
    aio_ring ring;
    struct spinlock lock;
    blockq bq;
    unsigned int nr;
    unsigned int ongoing_ops;
    unsigned int copied_evts;
    struct refcount refcount;
    closure_struct(thunk, free);
};

static struct aio *aio_alloc(process p, kernel_heaps kh, unsigned int *id)
{
    struct aio *aio = allocate(heap_locked(get_kernel_heaps()),
            sizeof(*aio));
    if (aio == INVALID_ADDRESS) {
        return 0;
    }
    process_lock(p);
    u64 aio_id = allocate_u64((heap)p->aio_ids, 1);
    if ((aio_id != INVALID_PHYSICAL) && !vector_set(p->aio, aio_id, aio)) {
        deallocate_u64((heap)p->aio_ids, aio_id, 1);
        aio_id = INVALID_PHYSICAL;
    }
    process_unlock(p);
    if (aio_id == INVALID_PHYSICAL) {
        deallocate(heap_locked(kh), aio, sizeof(*aio));
        return 0;
    }
    *id = (unsigned int) aio_id;
    aio->kh = kh;
    return aio;
}

static inline struct aio *aio_from_ring_id(process p, unsigned int id)
{
    process_lock(p);
    struct aio *aio = vector_get(p->aio, id);
    refcount_reserve(&aio->refcount);
    process_unlock(p);
    return aio;
}

closure_func_basic(thunk, void, aio_free)
{
    struct aio *aio = struct_from_closure(struct aio *, free);
    aio_ring ring = aio->ring;
    u64 phys = physical_from_virtual(ring);
    u64 alloc_size = pad(sizeof(*ring) + aio->nr * sizeof(struct io_event), PAGESIZE);
    unmap(u64_from_pointer(ring), alloc_size);
    deallocate_u64((heap) heap_physical(aio->kh), phys, alloc_size);
    deallocate(aio->vh, ring, alloc_size);
    deallocate(heap_locked(aio->kh), aio, sizeof(*aio));
}

sysreturn io_setup(unsigned int nr_events, aio_context_t *ctx_idp)
{
    if (!fault_in_user_memory(ctx_idp, sizeof(aio_context_t), true)) {
        return -EFAULT;
    }
    if (nr_events == 0) {
        return -EINVAL;
    }

    /* Allocate AIO ring structure and add it to process memory map.*/
    kernel_heaps kh = get_kernel_heaps();
    aio_ring ctx;
    nr_events += 1; /* needed because of head/tail management in ring buffer */
    u64 alloc_size = pad(sizeof(*ctx) + nr_events * sizeof(struct io_event),
            PAGESIZE);
    u64 phys = allocate_u64((heap) heap_physical(kh), alloc_size);
    if (phys == INVALID_PHYSICAL) {
        return -ENOMEM;
    }
    ctx = (aio_ring)process_map_physical(current->p, phys, alloc_size,
        VMAP_FLAG_READABLE | VMAP_FLAG_WRITABLE);
    if (ctx == INVALID_ADDRESS) {
        deallocate_u64((heap)heap_physical(kh), phys, alloc_size);
        return -ENOMEM;
    }

    struct aio *aio = aio_alloc(current->p, kh, &ctx->id);
    assert(aio);
    aio->vh = current->p->virtual;
    aio->ring = ctx;
    spin_lock_init(&aio->lock);
    aio->bq = 0;
    aio->nr = nr_events;
    aio->ongoing_ops = 0;
    init_refcount(&aio->refcount, 1, init_closure_func(&aio->free, thunk, aio_free));

    ctx->nr = nr_events;
    ctx->head = ctx->tail = 0;
    ctx->magic = AIO_RING_MAGIC;
    ctx->compat_features = 1;   /* same as Linux kernel */
    ctx->incompat_features = 0; /* same as Linux kernel */
    ctx->header_length = sizeof(*ctx);
    *ctx_idp = ctx;
    return 0;
}

closure_function(4, 1, void, aio_eventfd_complete,
                 heap, h, fdesc, f, u64 *, efd_val, context, proc_ctx,
                 sysreturn rv)
{
    u64 *efd_val = bound(efd_val);
    deallocate(bound(h), efd_val, sizeof(*efd_val));
    fdesc_put(bound(f));
    context_release_refcount(bound(proc_ctx));
    closure_finish();
}

closure_function(6, 1, void, aio_complete,
                 struct aio *, aio, fdesc, f, u64, data, u64, obj, int, res_fd, context, proc_ctx,
                 sysreturn rv)
{
    struct aio *aio = bound(aio);
    int res_fd = bound(res_fd);
    aio_ring ring = aio->ring;
    aio_lock(aio);
    aio->ongoing_ops--;
    unsigned int tail = ring->tail;
    if (tail >= aio->nr) {
        tail = 0;
    }
    ring->events[tail].data = bound(data);
    ring->events[tail].obj = bound(obj);
    ring->events[tail].res = rv;
    if (++tail == aio->nr) {
        tail = 0;
    }
    ring->tail = tail;
    blockq bq = aio->bq;
    if (bq)
        blockq_reserve(bq);
    aio_unlock(aio);
    fdesc_put(bound(f));
    context ctx = bound(proc_ctx);
    if (res_fd != AIO_RESFD_INVALID) {
        fdesc res = fdesc_get(((process_context)ctx)->p, res_fd);
        if (res) {
            if (res->write && fdesc_is_writable(res)) {
                heap h = heap_locked(aio->kh);
                u64 *efd_val = allocate(h, sizeof(*efd_val));
                assert(efd_val != INVALID_ADDRESS);
                *efd_val = 1;
                io_completion completion = closure(h, aio_eventfd_complete, h, res, efd_val, ctx);
                apply(res->write, efd_val, sizeof(*efd_val), 0, ctx, true, completion);
                ctx = 0;
            } else {
                fdesc_put(res);
            }
        }
    }
    if (bq) {
        blockq_wake_one(bq);
        blockq_release(bq);
    }
    closure_finish();
    refcount_release(&aio->refcount);
    if (ctx)
        context_release_refcount(ctx);
}

static unsigned int aio_avail_events(struct aio *aio)
{
    int avail = aio->ring->head - aio->ring->tail;
    if (avail <= 0) {
        avail += aio->nr;
    }
    return avail;
}

static sysreturn iocb_enqueue(struct aio *aio, struct iocb *iocb, context ctx)
{
    if (!validate_user_memory(iocb, sizeof(struct iocb), false) || context_set_err(ctx))
        return -EFAULT;

    if (iocb->aio_reserved1 || iocb->aio_reserved2 || !iocb->aio_buf ||
            (iocb->aio_flags & ~AIO_KNOWN_FLAGS)) {
        context_clear_err(ctx);
        return -EINVAL;
    }

    fdesc f = fdesc_get(current->p, iocb->aio_fildes);
    if (!f) {
        context_clear_err(ctx);
        return -EBADF;
    }
    int res_fd;
    if (iocb->aio_flags & IOCB_FLAG_RESFD)
        res_fd = iocb->aio_resfd;
    else
        res_fd = AIO_RESFD_INVALID;
    context_clear_err(ctx);
    aio_lock(aio);
    if (aio->ongoing_ops >= aio_avail_events(aio) - 1) {
        aio_unlock(aio);
        fdesc_put(f);
        return -EAGAIN;
    }
    aio->ongoing_ops++;
    aio_unlock(aio);
    process_context pc = INVALID_ADDRESS;
    io_completion completion = INVALID_ADDRESS;
    refcount_reserve(&aio->refcount);
    sysreturn rv;
    if (context_set_err(ctx)) {
        rv = -EFAULT;
        goto error;
    }
    pc = get_process_context();
    if (pc == INVALID_ADDRESS) {
        rv = -ENOMEM;
        goto error;
    }
    completion = closure(heap_locked(aio->kh), aio_complete, aio, f, iocb->aio_data, (u64)iocb,
                         res_fd, &pc->uc.kc.context);
    if (completion == INVALID_ADDRESS) {
        rv = -ENOMEM;
        goto error;
    }
    switch (iocb->aio_lio_opcode) {
    case IOCB_CMD_PREAD:
        if (!f->read) {
            rv = -EINVAL;
            goto error;
        } else if (!fdesc_is_readable(f)) {
            rv = -EBADF;
            goto error;
        }
        apply(f->read, (void *) iocb->aio_buf, iocb->aio_nbytes,
              iocb->aio_offset, &pc->uc.kc.context, true, completion);
        break;
    case IOCB_CMD_PWRITE:
        if (!f->write) {
            rv = -EINVAL;
            goto error;
        } else if (!fdesc_is_writable(f)) {
            rv = -EBADF;
            goto error;
        }
        apply(f->write, (void *) iocb->aio_buf, iocb->aio_nbytes,
              iocb->aio_offset, &pc->uc.kc.context, true, completion);
        break;
    default:
        rv = -EINVAL;
        goto error;
    }
    context_clear_err(ctx);
    return 0;
error:
    if (rv != -EFAULT)
        context_clear_err(ctx);
    aio_lock(aio);
    aio->ongoing_ops--;
    aio_unlock(aio);
    refcount_release(&aio->refcount);
    if (completion != INVALID_ADDRESS)
        deallocate_closure(completion);
    if (pc != INVALID_ADDRESS)
        context_release_refcount(&pc->uc.kc.context);
    fdesc_put(f);
    return rv;
}

sysreturn io_submit(aio_context_t ctx_id, long nr, struct iocb **iocbpp)
{
    struct aio *aio;
    context ctx = get_current_context(current_cpu());
    if (!validate_user_memory(ctx_id, sizeof(struct aio_ring), false) ||
        !validate_user_memory(iocbpp, sizeof(struct iocb *) * nr, false) ||
        context_set_err(ctx))
        return -EFAULT;
    aio = aio_from_ring_id(current->p, ctx_id->id);
    context_clear_err(ctx);
    if (!aio)
        return -EINVAL;
    int io_ops;
    for (io_ops = 0; io_ops < nr; io_ops++) {
        struct iocb *iocbp;
        sysreturn rv;
        if (!context_set_err(ctx)) {
            iocbp = iocbpp[io_ops];
            context_clear_err(ctx);
            rv = iocb_enqueue(aio, iocbp, ctx);
        } else  {
            rv = -EFAULT;
        }
        if (rv) {
            if (io_ops == 0) {
                io_ops = rv;
            }
            break;
        }
    }
    refcount_release(&aio->refcount);
    return io_ops;
}

/* Called with aio lock held (unless BLOCKQ_ACTION_BLOCKED is set in flags);
 * returns with aio lock released. */
closure_function(6, 1, sysreturn, io_getevents_bh,
                 struct aio *, aio, long, min_nr, long, nr, struct io_event *, events, timestamp, timeout, io_completion, completion,
                 u64 flags)
{
    struct aio *aio = bound(aio);
    struct io_event *events = bound(events);
    timestamp timeout = bound(timeout);
    aio_ring ring = aio->ring;
    sysreturn rv;
    if (flags & BLOCKQ_ACTION_BLOCKED)
        aio_lock(aio);
    if (flags & BLOCKQ_ACTION_NULLIFY) {
        rv = (timeout == infinity) ? -ERESTARTSYS : -EINTR;
        goto out;
    }

    unsigned int head = ring->head;
    unsigned int tail = ring->tail;
    if (head >= aio->nr) {
        head = 0;
    }
    if (tail >= aio->nr) {
        tail = 0;
    }
    context ctx = get_current_context(current_cpu());
    if (context_set_err(ctx)) {
        rv = -EFAULT;
        goto out;
    }
    while (head != tail) {
        if (events) {
            runtime_memcpy(&events[aio->copied_evts], &ring->events[head],
                    sizeof(struct io_event));
        }
        if (++head == aio->nr) {
            head = 0;
        }
        if (++aio->copied_evts == bound(nr)) {
            break;
        }
    }
    context_clear_err(ctx);
    ring->head = head;
    ring->tail = tail;
    if ((aio->copied_evts < bound(min_nr)) && (timeout != 0) &&
            !(flags & BLOCKQ_ACTION_TIMEDOUT)) {
        aio_unlock(aio);
        return blockq_block_required((unix_context)ctx, flags);
    }
    rv = aio->copied_evts;
out:
    aio->bq = 0;
    aio_unlock(aio);
    apply(bound(completion), rv);
    closure_finish();
    refcount_release(&aio->refcount);
    return rv;
}

sysreturn io_getevents(aio_context_t ctx_id, long min_nr, long nr,
        struct io_event *events, struct timespec *timeout)
{
    context ctx = get_current_context(current_cpu());
    if (!validate_user_memory(ctx_id, sizeof(struct aio_ring), false) ||
        !validate_user_memory(events, sizeof(struct io_event) * nr, true) ||
        (timeout && !validate_user_memory(timeout, sizeof(struct timespec), false)) ||
        context_set_err(ctx))
        return -EFAULT;
    struct aio *aio = aio_from_ring_id(current->p, ctx_id->id);
    timestamp ts = timeout ? time_from_timespec(timeout) : infinity;
    context_clear_err(ctx);
    if ((nr <= 0) || (nr < min_nr) || !aio)
        return -EINVAL;
    aio_lock(aio);
    aio->copied_evts = 0;
    aio->bq = current->thread_bq;
    return blockq_check_timeout(aio->bq,
                                contextual_closure(io_getevents_bh, aio, min_nr, nr, events,
                                                   ts, syscall_io_complete), false,
                                CLOCK_ID_MONOTONIC, (ts == infinity) ? 0 : ts, false);
}

static sysreturn io_destroy_internal(struct aio *aio, thread t, boolean in_bh);

closure_function(2, 1, void, io_destroy_complete,
                 struct aio *, aio, thread, t,
                 sysreturn rv)
{
    struct aio *aio = bound(aio);
    if (aio->ongoing_ops) {
        /* This can happen if io_getevents has been interrupted by a signal: try
         * again. */
        io_destroy_internal(aio, bound(t), true);
    } else {
        refcount_release(&aio->refcount);
        apply(syscall_io_complete, 0);
    }
    closure_finish();
}

static sysreturn io_destroy_internal(struct aio *aio, thread t, boolean in_bh)
{
    io_completion completion = closure(heap_locked(aio->kh),
                                       io_destroy_complete, aio, t);
    assert(completion != INVALID_ADDRESS);
    aio_lock(aio);
    unsigned int ongoing_ops = aio->ongoing_ops;
    if (ongoing_ops) {
        aio->copied_evts = 0;
        aio->bq = t->thread_bq;
        refcount_reserve(&aio->refcount);
        return blockq_check(aio->bq,
                            contextual_closure(io_getevents_bh, aio,
                                               ongoing_ops, ongoing_ops, 0,
                                               infinity, completion), in_bh);
    } else {
        aio_unlock(aio);
        apply(completion, 0);
        return 0;
    }
}

sysreturn io_destroy(aio_context_t ctx_id)
{
    unsigned int id;
    if (!get_user_value(&ctx_id->id, &id))
        return -EFAULT;
    process p = current->p;
    process_lock(p);
    struct aio *aio = vector_get(p->aio, id);
    if (aio) {
        assert(vector_set(p->aio, id, 0));
        deallocate_u64((heap) p->aio_ids, id, 1);
    }
    process_unlock(p);
    if (!aio)
        return -EINVAL;
    return io_destroy_internal(aio, current, false);
}
