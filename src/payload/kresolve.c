#include "kresolve.h"

struct kr_range {
    u64 start;
    u64 len;
};

static inline u16 rd16(u64 a) {
    const u8 *p = (const u8 *)a;
    return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 rd32(u64 a) {
    const u8 *p = (const u8 *)a;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline s32 rds32(u64 a) {
    return (s32)rd32(a);
}

static inline u64 rd64(u64 a) {
    return (u64)rd32(a) | ((u64)rd32(a + 4) << 32);
}

u64 kr_find_base(void) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000082));
    u64 lstar = ((u64)hi << 32) | lo;
    if ((lstar >> 48) != 0xffff) return 0;

    u64 cand = lstar & ~0x3fffUL;
    for (int step = 0; step <= 16; step++) {
        /* ELF64 little-endian FreeBSD kernel image header (\x7fELF, class=2, data=1, ver=1, OSABI=9). */
        if (rd64(cand) == 0x09010102464c457fUL &&
            rd64(cand + 0x20) == 0x40UL &&
            rd16(cand + 0x36) == 0x38) {
            u16 phnum = rd16(cand + 0x38);
            if (phnum >= 1 && phnum <= 32) return cand;
        }
        cand -= 0x4000UL;
    }
    return 0;
}

static u32 kr_ro_ranges(u64 base, struct kr_range *out, u32 cap) {
    u64 phoff = rd64(base + 0x20);
    u16 phentsz = rd16(base + 0x36);
    u16 phnum = rd16(base + 0x38);
    u32 count = 0;
    for (u16 i = 0; i < phnum && count < cap; i++) {
        u64 ph = base + phoff + (u64)i * phentsz;
        u32 ptype = rd32(ph);
        u32 pflags = rd32(ph + 4);
        u64 pva = rd64(ph + 16);
        u64 pfsz = rd64(ph + 32);
        if (ptype == 1 && !(pflags & 2) && pva >= base && pfsz > 0 && pfsz <= 0x4000000UL) {
            out[count].start = pva;
            out[count].len = pfsz;
            count++;
        }
    }
    return count;
}

int kr_rx_bounds(u64 base, u64 *start, u64 *len) {
    u64 phoff = rd64(base + 0x20);
    u16 phentsz = rd16(base + 0x36);
    u16 phnum = rd16(base + 0x38);
    for (u16 i = 0; i < phnum; i++) {
        u64 ph = base + phoff + (u64)i * phentsz;
        u32 ptype = rd32(ph);
        u32 pflags = rd32(ph + 4);
        u64 pva = rd64(ph + 16);
        u64 pfsz = rd64(ph + 32);
        if (ptype == 1 && (pflags & 1) && pva >= base && pfsz >= 0x100000UL && pfsz <= 0x4000000UL) {
            *start = pva;
            *len = pfsz;
            return 0;
        }
    }
    return -1;
}

static u64 scan_first(u64 start, u64 len, const s16 *pat, u64 plen) {
    if (!plen || len < plen) return 0;
    const u8 *p = (const u8 *)start;
    u64 limit = len - plen + 1;
    s16 first = pat[0];
    for (u64 i = 0; i < limit; i++) {
        if (first >= 0 && p[i] != (u8)first) continue;
        int ok = 1;
        for (u64 k = 1; k < plen; k++) {
            if (pat[k] >= 0 && p[i + k] != (u8)pat[k]) {
                ok = 0;
                break;
            }
        }
        if (ok) return start + i;
    }
    return 0;
}

static u64 scan_unique(u64 start, u64 len, const s16 *pat, u64 plen) {
    u64 first = scan_first(start, len, pat, plen);
    if (!first) return 0;
    u64 next = first + 1;
    u64 end = start + len;
    if (next < end && scan_first(next, end - next, pat, plen)) return 0;
    return first;
}

static u64 find_str_unique(const struct kr_range *ro, u32 n_ro, const char *s) {
    u64 slen = 0;
    while (s[slen]) slen++;
    slen++;
    if (slen > 64) return 0;
    s16 pat[64];
    for (u64 i = 0; i < slen; i++) pat[i] = (u8)s[i];
    u64 match = 0;
    for (u32 r = 0; r < n_ro; r++) {
        u64 pos = ro[r].start;
        u64 end = ro[r].start + ro[r].len;
        while (pos < end) {
            u64 hit = scan_first(pos, end - pos, pat, slen);
            if (!hit) break;
            if (match) return 0;
            match = hit;
            pos = hit + 1;
        }
    }
    return match;
}

static u64 walk_prologue(u64 addr, u64 min_addr, u64 max_back) {
    for (u64 d = 0; d <= max_back && addr >= min_addr + d; d++) {
        u64 p = addr - d;
        if (rd32(p) != 0xe5894855U) continue;
        if (p == min_addr || (p & 0xf) == 0) return p;
        u8 prev = *(const u8 *)(p - 1);
        if (prev == 0x90 || prev == 0xc3 || prev == 0xcc) return p;
    }
    return 0;
}

/* First RIP-relative lea in [start, start + len) resolving to target. */
static u64 find_lea_ref(u64 start, u64 len, u64 target) {
    if (len < 7) return 0;
    const u8 *p = (const u8 *)start;
    for (u64 i = 1; i + 6 <= len; i++) {
        if (p[i] != 0x8d) continue;
        if ((p[i + 1] & 0xc7) != 0x05) continue;
        u8 rex = p[i - 1];
        if ((rex & 0xf0) != 0x40) continue;
        u64 next_ip = start + i + 6;
        if (next_ip + (u64)(s64)rds32(start + i + 2) == target) return start + i - 1;
    }
    return 0;
}

/* Locate the thin wrapper right before helper that calls helper directly. */
static u64 find_wrapper_before(u64 helper, u64 min_addr) {
    u64 wstart = (helper > min_addr + 0x60) ? (helper - 0x60) : min_addr;
    for (u64 a = wstart; a + 5 <= helper; a++) {
        if (*(const u8 *)a != 0xe8) continue;
        if (a + 5 + (u64)(s64)rds32(a + 1) == helper) {
            u64 fn = walk_prologue(a, wstart, 0x20);
            if (fn) return fn;
        }
    }
    return 0;
}

static int scan_has_err(u64 start, u64 end) {
    static const s16 e1[] = {0xba, 0x01, 0x34, 0x2f, 0x80};
    static const s16 e3[] = {0xba, 0x03, 0x34, 0x2f, 0x80};
    return scan_first(start, end - start, e1, sizeof(e1) / sizeof(e1[0])) ||
           scan_first(start, end - start, e3, sizeof(e3) / sizeof(e3[0]));
}

u64 kr_resolve_copyout(u64 rx_start, u64 rx_len) {
    /* John Tornblom's PS4 copyout pattern (kexec_find_copyout). */
    static const s16 pat[] = {
        0x55, 0x48, 0x89, 0xe5, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x20, 0x00, 0x00, 0x00,
        0x4c, 0x8b, 0x05, -1, -1, -1, -1, 0x4c, 0x89, 0x80, 0xd0, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xd2, 0x74, -1, 0x65, 0x48, 0x8b, 0x0c, 0x25, 0x00, 0x00, 0x00,
        0x00, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0x88, -1, -1, -1, -1,
        0x48, 0xf7, 0xc1, 0x00, 0x00, 0x00, 0x40
    };
    return scan_unique(rx_start, rx_len, pat, sizeof(pat) / sizeof(pat[0]));
}

static int scan_accept_pat(u64 rx_start, u64 end, const s16 *pat, u64 *match) {
    u64 pos = rx_start;
    while (pos < end) {
        u64 hit = scan_first(pos, end - pos, pat, 8);
        if (!hit) break;
        u64 w_end = (hit + 96 < end) ? (hit + 96) : end;
        if (hit + 16 < w_end && scan_has_err(hit + 16, w_end)) {
            if (*match && *match != hit) return -1;
            *match = hit;
        }
        pos = hit + 1;
    }
    return 0;
}

u64 kr_resolve_accept_site(u64 rx_start, u64 rx_len) {
    /* Accept either unpatched (cmp dword [rax+0x20],0x40; jne +??; mov ..., [r12..])
     * or already-patched (test byte [rax+0x20],0x48; je +??; mov ..., [r12..]) form,
     * anchored by the Sony BT error codes (0x802f3401/0x802f3403) right after it. */
    static const s16 orig[] = {0x83, 0x78, 0x20, 0x40, 0x75, -1, 0x49, 0x8b};
    static const s16 patched[] = {0xf6, 0x40, 0x20, 0x48, 0x74, -1, 0x49, 0x8b};
    u64 end = rx_start + rx_len;
    u64 match = 0;
    if (scan_accept_pat(rx_start, end, orig, &match)) return 0;
    if (scan_accept_pat(rx_start, end, patched, &match)) return 0;
    return match;
}

static u64 resolve_lock_wrapper(u64 bt_start, u64 bt_len, const struct kr_range *ro, u32 n_ro, const char *name) {
    u64 s = find_str_unique(ro, n_ro, name);
    if (!s) return 0;
    u64 ref = find_lea_ref(bt_start, bt_len, s);
    if (!ref) return 0;
    u64 helper = walk_prologue(ref, bt_start, 0x80);
    if (!helper) return 0;
    return find_wrapper_before(helper, bt_start);
}

static int resolve_cmd_and_fmt(u64 bt_start, u64 bt_len, struct bt_koff *out) {
    static const s16 err_0b02[] = {0x41, 0xbd, 0x02, 0x0b, 0x2f, 0x80};
    static const s16 call_a[] = {0xbe, 0x0f, 0x04, 0x00, 0x00, 0x41, 0xb8, 0x18, 0x3c, 0x00, 0x00};
    static const s16 call_b[] = {0x41, 0xb8, 0x18, 0x3c, 0x00, 0x00, 0xbe, 0x0f, 0x04, 0x00, 0x00};

    u64 e = scan_unique(bt_start, bt_len, err_0b02, sizeof(err_0b02) / sizeof(err_0b02[0]));
    if (!e) return -1;
    u64 cmd_fn = walk_prologue(e, bt_start, 0xe0);
    if (!cmd_fn) return -1;

    u64 c3c18 = scan_unique(bt_start, bt_len, call_a, sizeof(call_a) / sizeof(call_a[0]));
    if (!c3c18) c3c18 = scan_unique(bt_start, bt_len, call_b, sizeof(call_b) / sizeof(call_b[0]));
    if (!c3c18) return -1;

    /* Locate `lea rdx, [rip + disp32]` -> "22\0" near the 0x40f / 0x3c18 call setup. */
    u64 fmt = 0;
    u64 w0 = (c3c18 > bt_start + 0x24) ? (c3c18 - 0x24) : bt_start;
    u64 w1 = (c3c18 + 0x24 < bt_start + bt_len) ? (c3c18 + 0x24) : (bt_start + bt_len);
    for (u64 a = w0; a + 7 <= w1; a++) {
        if (*(const u8 *)a == 0x48 && *(const u8 *)(a + 1) == 0x8d && *(const u8 *)(a + 2) == 0x15) {
            u64 cand = (a + 7) + (u64)(s64)rds32(a + 3);
            if ((cand >> 48) == 0xffff &&
                *(const u8 *)cand == '2' && *(const u8 *)(cand + 1) == '2' && *(const u8 *)(cand + 2) == 0) {
                fmt = cand;
                break;
            }
        }
    }
    if (!fmt) return -1;

    /* Confirm control flow from c3c18 reaches cmd_fn (direct or via one short jmp). */
    u64 p = c3c18 + 11;
    u64 pend = p + 0x40;
    int jumped = 0;
    int confirmed = 0;
    while (p + 5 <= pend && p + 5 <= bt_start + bt_len) {
        u8 op = *(const u8 *)p;
        if (!jumped && op == 0xeb) {
            p = p + 2 + (s64)*(const s8 *)(p + 1);
            pend = p + 0x20;
            jumped = 1;
            continue;
        }
        if ((op == 0xe8 || op == 0xe9) && (p + 5 + (u64)(s64)rds32(p + 1) == cmd_fn)) {
            confirmed = 1;
            break;
        }
        p++;
    }
    if (!confirmed) return -1;

    /* Optionally refine policy/state offsets from the surrounding fn_3c18 function. */
    u64 fn3_start = walk_prologue(c3c18, bt_start, 0x180);
    if (fn3_start) {
        static const s16 pol_pat[] = {0x81, 0xb8, -1, -1, 0x00, 0x00, 0x18, 0x3c, 0x00, 0x00};
        u64 pol_hit = scan_first(fn3_start, c3c18 - fn3_start, pol_pat, sizeof(pol_pat) / sizeof(pol_pat[0]));
        if (pol_hit) out->conn_policy_off = rd32(pol_hit + 2);
        static const s16 st_pat[] = {0x83, 0xb8, -1, -1, 0x00, 0x00, 0x03};
        u64 st_hit = scan_first(fn3_start, c3c18 - fn3_start, st_pat, sizeof(st_pat) / sizeof(st_pat[0]));
        if (st_hit) out->conn_state_off = rd32(st_hit + 2);
        /* Scan [fn3_start, fn3_start + 0x20] for `mov rcx, [rdi + dev_off]` (48 8b 8f/87 disp32). */
        for (u64 k = fn3_start; k + 7 <= fn3_start + 0x20; k++) {
            if (*(const u8 *)k == 0x48 && *(const u8 *)(k + 1) == 0x8b &&
                (*(const u8 *)(k + 2) == 0x8f || *(const u8 *)(k + 2) == 0x87)) {
                u32 doff = rd32(k + 3);
                if (doff >= 0x100 && doff <= 0x1500) { out->conn_dev_off = doff; break; }
            }
        }
        /* Scan [w0, w1] for `movzx ecx, word ptr [rax + handle_off]` (0f b7 48 off8). */
        for (u64 k = w0; k + 4 <= w1; k++) {
            if (*(const u8 *)k == 0x0f && *(const u8 *)(k + 1) == 0xb7 && *(const u8 *)(k + 2) == 0x48) {
                out->conn_handle_off = *(const u8 *)(k + 3);
                break;
            }
        }
    }

    out->cmd_fn = cmd_fn;
    out->fmt_str = fmt;
    return 0;
}

static int resolve_cache_and_bounds(u64 bt_start, u64 bt_len, struct bt_koff *out) {
    static const s16 pat_3200[] = {0x41, 0xbd, 0x00, 0x32, 0x00, 0x00};
    static const s16 pat_0107[] = {0x41, 0xbc, 0x07, 0x01, 0x2f, 0x80};

    u64 hit = scan_first(bt_start, bt_len, pat_3200, sizeof(pat_3200) / sizeof(pat_3200[0]));
    if (!hit) return -1;
    u64 cache_fn = walk_prologue(hit, bt_start, 0xa0);
    if (!cache_fn) return -1;
    out->cache_fn = cache_fn;

    /* Extract/verify cache array bounds & stride from name_lookup (0x802f0107) if matching. */
    u64 nl = scan_unique(bt_start, bt_len, pat_0107, sizeof(pat_0107) / sizeof(pat_0107[0]));
    if (nl && nl >= bt_start + 0x18 && nl + 0x30 <= bt_start + bt_len) {
        u64 fn = walk_prologue(nl, bt_start, 0x30);
        if (fn) {
            u32 c_off = 0, c_end = 0, c_stride = 0;
            for (u64 k = fn; k + 7 <= nl; k++) {
                if (*(const u8 *)k == 0x48 && *(const u8 *)(k + 1) == 0x8d && *(const u8 *)(k + 2) == 0x9f)
                    c_off = rd32(k + 3);
                else if (*(const u8 *)k == 0x48 && *(const u8 *)(k + 1) == 0x81 && *(const u8 *)(k + 2) == 0xc7)
                    c_end = rd32(k + 3);
            }
            for (u64 k = nl; k + 7 <= nl + 0x30; k++) {
                if (*(const u8 *)k == 0x48 && *(const u8 *)(k + 1) == 0x81 && *(const u8 *)(k + 2) == 0xc3) {
                    c_stride = rd32(k + 3);
                    break;
                }
            }
            if (c_off && c_end > c_off && c_stride >= 0x100 && c_stride <= 0x2000 &&
                ((c_end - c_off) % c_stride) == 0) {
                out->cache_off = c_off;
                out->cache_end = c_end;
                out->cache_stride = c_stride;
            }
        }
    }
    return 0;
}

static u64 resolve_ctx_ptr(u64 bt_start, u64 bt_len, const struct kr_range *ro, u32 n_ro) {
    static const s16 store_pat[] = {
        0xba, 0x01, 0x01, 0x00, 0x00,
        0xe8, -1, -1, -1, -1,
        0x48, 0x85, 0xc0,
        0x48, 0x89, 0x05, -1, -1, -1, -1
    };
    u64 s_drv = find_str_unique(ro, n_ro, "SceBtDriver");
    if (!s_drv) return 0;
    u64 ref_drv = find_lea_ref(bt_start, bt_len, s_drv);
    if (!ref_drv) return 0;
    u64 w = (ref_drv > bt_start + 0xa0) ? (ref_drv - 0xa0) : bt_start;
    u64 hit = scan_first(w, ref_drv - w, store_pat, sizeof(store_pat) / sizeof(store_pat[0]));
    if (!hit) return 0;
    u64 ctx_ptr = (hit + 20) + (u64)(s64)rds32(hit + 16);
    return ((ctx_ptr >> 48) == 0xffff) ? ctx_ptr : 0;
}

int kr_resolve_bt(u64 base, struct bt_koff *out) {
    for (u64 i = 0; i < sizeof(*out); i++) ((volatile u8 *)out)[i] = 0;

    u64 rx_start = 0, rx_len = 0;
    if (kr_rx_bounds(base, &rx_start, &rx_len)) { out->fail_stage = 1; return -1; }

    struct kr_range ro[4];
    u32 n_ro = kr_ro_ranges(base, ro, 4);
    if (!n_ro) { ro[0].start = rx_start; ro[0].len = rx_len; n_ro = 1; }

    out->base = base;
    out->copyout_fn = kr_resolve_copyout(rx_start, rx_len);
    if (!out->copyout_fn) { out->fail_stage = 2; return -1; }

    u64 site = kr_resolve_accept_site(rx_start, rx_len);
    if (!site) { out->fail_stage = 3; return -1; }
    out->accept_site = site;

    u64 rx_end = rx_start + rx_len;
    u64 bt_start = (site > rx_start + 0x60000UL) ? (site - 0x60000UL) : rx_start;
    u64 bt_end = (site + 0x60000UL < rx_end) ? (site + 0x60000UL) : rx_end;
    u64 bt_len = bt_end - bt_start;

    /* Default known struct layout, refined/verified dynamically below where matched. */
    out->cache_off = 0x18da8;
    out->cache_end = 0x20ea8;
    out->cache_stride = 0x408;
    out->conn_off = 0x42a8;
    out->conn_end = 0xd960;
    out->conn_stride = 0x1588;
    out->conn_dev_off = 0x638;
    out->conn_state_off = 0x1560;
    out->conn_policy_off = 0x1564;
    out->conn_handle_off = 0x48;

    out->lock_fn = resolve_lock_wrapper(bt_start, bt_len, ro, n_ro, "bt_sdio_lock");
    if (!out->lock_fn) { out->fail_stage = 4; return -1; }

    out->unlock_fn = resolve_lock_wrapper(bt_start, bt_len, ro, n_ro, "bt_sdio_unlock");
    if (!out->unlock_fn) { out->fail_stage = 5; return -1; }

    if (resolve_cmd_and_fmt(bt_start, bt_len, out)) { out->fail_stage = 6; return -1; }
    if (resolve_cache_and_bounds(bt_start, bt_len, out)) { out->fail_stage = 7; return -1; }
    out->ctx_ptr = resolve_ctx_ptr(bt_start, bt_len, ro, n_ro);
    if (!out->ctx_ptr) { out->fail_stage = 8; return -1; }

    out->valid = 1;
    return 0;
}
