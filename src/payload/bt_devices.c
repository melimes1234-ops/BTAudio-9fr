/* Firmware-independent locked snapshot of the driver's existing Classic device cache,
 * and the normal HCI packet-type request used by the proven BTA bridge.
 * Kernel symbols and bounds are discovered dynamically and cached per worker run. */
#include "protocol.h"
#include "kresolve.h"

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long o;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:"
                     : "=a"(o), "+d"(c), "+D"(a), "+S"(b)
                     : "a"(n)
                     : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return o;
}

struct resolve_args {
    void *fn;
    struct bt_koff *out;
};

struct input {
    u64 address;
    int mode;
    const struct bt_koff *k;
    struct bt_devices *out;
    struct bt_prep_diag *diag;
};
struct args {
    void *fn;
    struct input *input;
};

static struct bt_koff g_koff;

static void zero_mem(void *p, u64 n) {
    volatile u8 *b = (volatile u8 *)p;
    for (u64 i = 0; i < n; i++) b[i] = 0;
}

static int resolve_cb(void *td, struct resolve_args *a) {
    (void)td;
    u64 base = kr_find_base();
    if (!base) return 101;
    struct bt_koff res;
    int bad = kr_resolve_bt(base, &res);
    if (!res.copyout_fn) return 102;
    if (*(const u32 *)res.copyout_fn != 0xe5894855U) return 103;
    int (*copyout)(const void *, void *, u64) = (void *)res.copyout_fn;
    int cprc = copyout(&res, a->out, sizeof(res));
    return bad ? 102 : cprc;
}

static int callback(void *td, struct args *a) {
    (void)td;
    struct input *in = a->input;
    const struct bt_koff *k = in ? in->k : 0;

    u64 base = kr_find_base();
    if (!base) return 22;
    if (!k || !k->valid || k->base != base) return 22;
    if (*(const u32 *)k->cache_fn != 0xe5894855U || *(const u32 *)k->cmd_fn != 0xe5894855U ||
        *(const u32 *)k->copyout_fn != 0xe5894855U) {
        return 22;
    }

    /* Input is the calling worker's private stack, as in existing syscall-11 helpers. */
    u64 address = in->address;
    int mode = in->mode;
    struct bt_devices *destination = in->out;
    if (mode != 0 && mode != 1) return 22;
    if (mode == 1 && (!address || address >> 48)) return 22;

    struct bt_devices out;
    zero_mem(&out, sizeof(out));

    int (*lock)(void) = (void *)k->lock_fn;
    int (*unlock)(int) = (void *)k->unlock_fn;
    int rc = lock();
    if (rc) return rc;

    unsigned char *ctx = *(void **)k->ctx_ptr;
    if (!ctx || ((u64)ctx >> 48) != 0xffff) {
        unlock(0);
        return 6;
    }

    if (mode == 0) {
        for (unsigned off = k->cache_off; off < k->cache_end; off += k->cache_stride) {
            unsigned char *d = ctx + off;
            u64 mac = *(u64 *)d;
            if (!mac || mac >> 48) continue;
            if (out.count < BT_DEVICE_LIMIT) {
                struct bt_device *v = &out.items[out.count++];
                v->address = mac;
                v->cod = *(unsigned *)(d + 8);
                v->services = *(unsigned *)(d + 0x20);
                v->flags = *(unsigned *)(d + 0x24);
                for (unsigned i = 0; i < sizeof(v->name) - 1; i++) {
                    unsigned char c = d[0x80 + i];
                    if (!c) break;
                    v->name[i] = (c >= 32 && c < 127) ? c : '?';
                }
            }
        }
    } else {
        struct bt_prep_diag d_out;
        zero_mem(&d_out, sizeof(d_out));
        rc = 2;
        for (unsigned off = k->conn_off; off < k->conn_end; off += k->conn_stride) {
            unsigned char *c = ctx + off;
            if (*(u64 *)(c + 0x30) != address) continue;
            unsigned char *d = *(void **)(c + k->conn_dev_off);
            d_out.conn_state = *(unsigned *)(c + k->conn_state_off);
            d_out.conn_policy = *(unsigned *)(c + k->conn_policy_off);
            d_out.handle = *(unsigned short *)(c + k->conn_handle_off);
            if (!d || ((u64)d >> 48) != 0xffff) {
                rc = 93;
                break;
            }
            d_out.dev_services = *(unsigned *)(d + 0x20);
            if (!(d_out.dev_services & 8)) {
                rc = 93;
                break;
            }
            if (d_out.conn_state >= 3) {
                d_out.skipped = 1;
                rc = 0;
                break;
            }
            int (*command)(void *, unsigned, const char *, ...) = (void *)k->cmd_fn;
            rc = command(ctx, 0x40f, (const char *)k->fmt_str, (unsigned)d_out.handle, 0x3c18);
            if (rc >= 0) {
                *(unsigned *)(c + k->conn_policy_off) = 0x3c18;
                d_out.conn_policy = 0x3c18;
                rc = 0;
            }
            break;
        }
        d_out.rc = rc;
        unlock(0);
        if (in->diag) {
            int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
            int cprc = copyout(&d_out, in->diag, sizeof(d_out));
            if (cprc && !rc) rc = cprc;
        }
        return rc;
    }

    unlock(0);
    if (mode == 0) {
        int (*copyout)(const void *, void *, u64) = (void *)k->copyout_fn;
        return copyout(&out, destination, sizeof(out));
    }
    return rc;
}

static int invoke(struct input *in) {
    if (!g_koff.valid) {
        long r = sc3(11, (long)resolve_cb, (long)&g_koff, 0);
        if (r || !g_koff.valid) return r ? (int)r : 22;
    }
    in->k = &g_koff;
    return (int)sc3(11, (long)callback, (long)in, 0);
}

int bt_list(struct bt_devices *out) {
    struct input in = {.mode = 0, .out = out};
    return invoke(&in);
}

int bt_prepare_diag(u64 address, struct bt_prep_diag *diag) {
    struct input in = {.address = address, .mode = 1, .diag = diag};
    return invoke(&in);
}

int bt_prepare(u64 address) {
    return bt_prepare_diag(address, 0);
}

const struct bt_koff *bt_koff_info(void) {
    return &g_koff;
}
