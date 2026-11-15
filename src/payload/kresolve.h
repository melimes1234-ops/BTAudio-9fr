#ifndef BTA_KRESOLVE_H
#define BTA_KRESOLVE_H

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
typedef short s16;
typedef signed char s8;
typedef long s64;

struct bt_koff {
    u64 valid;
    u64 base;
    u64 copyout_fn;
    u64 lock_fn;
    u64 unlock_fn;
    u64 cmd_fn;
    u64 cache_fn;
    u64 ctx_ptr;
    u64 fmt_str;
    u32 cache_off;
    u32 cache_end;
    u32 cache_stride;
    u32 conn_off;
    u32 conn_end;
    u32 conn_stride;
    u32 conn_dev_off;
    u32 conn_state_off;
    u32 conn_policy_off;
    u32 conn_handle_off;
    u64 accept_site;
    u64 fail_stage;
};

u64 kr_find_base(void);
int kr_rx_bounds(u64 base, u64 *start, u64 *len);
u64 kr_resolve_copyout(u64 rx_start, u64 rx_len);
u64 kr_resolve_accept_site(u64 rx_start, u64 rx_len);
int kr_resolve_bt(u64 base, struct bt_koff *out);

#endif
