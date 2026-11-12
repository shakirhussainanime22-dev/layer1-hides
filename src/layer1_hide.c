/*
 * layer1-hide - Hide root / APatch / Magisk / Xposed / GameGuardian
 * filesystem paths from non-root processes.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <kpmodule.h>
#include <hook.h>
#include <syscall.h>
#include <kputils.h>

/* Manual definitions to avoid missing standard headers */
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef __user
#define __user
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Hardcoded ARM64 Syscall Numbers */
#ifndef __NR_openat
#define __NR_openat 56
#endif
#ifndef __NR_faccessat
#define __NR_faccessat 48
#endif
#ifndef __NR_newfstatat
#define __NR_newfstatat 79
#endif
#ifndef __NR_statx
#define __NR_statx 291
#endif
#ifndef __NR_readlinkat
#define __NR_readlinkat 78
#endif
#ifndef __NR_getdents64
#define __NR_getdents64 61
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif

/* Function pointers for kernel symbols */
static int (*kp_printk_fn)(const char *fmt, ...);
#define pr_info(fmt, ...) kp_printk_fn(fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)  kp_printk_fn(fmt, ##__VA_ARGS__)

static unsigned int (*kp_current_uid_fn)(void);
static unsigned long (*kp_copy_from_user_fn)(void *to, const void __user *from, unsigned long n);
static unsigned long (*kp_copy_to_user_fn)(void __user *to, const void *from, unsigned long n);

/* ------------------------------------------------------------------ */
/* Hide list: edit here. One entry per line. Substring match.          */
/* ------------------------------------------------------------------ */
static const char *hide_list[] = {
    "/data/adb/",                  
    "/data/adb/modules",           
    "/data/adb/ap/",               
    "/data/adb/apd",              
    "/system/bin/su",
    "/system/xbin/su",
    "/sbin/su",
    "magisk",                      
    "Magisk",                      
    "xposed",                      
    "Xposed",                      
    "riru",                        
    "zygisk",                      
    "busybox",                     
    "/sbin/.magisk/",
    "/debug_ramdisk/",
    "/data/local/tmp/GG/",
    "/data/local/tmp/gameguardian/",
    "/sdcard/GameGuardian/",
    "/data/media/0/GameGuardian/",
    "/data/data/com.sand.air/",
    "/data/data/catch_.me_.if_.you_.can_/",
    "/data/data/com.hsjjolddvjeac/",       
    "/sdcard/Android/data/com.hsjjolddvjeac/", 
    NULL
};

#define L1_MAX_EXTRA 16
#define L1_EXTRA_LEN 64
#define L1_GETDENTS_BUF 4096
#define L1_ARGS_BUF 512

static char extra_hide[L1_MAX_EXTRA][L1_EXTRA_LEN];
static int extra_count;
static int getdents_too_large_logged;

static int h_openat, h_openat2, h_faccessat, h_faccessat2;
static int h_newfstatat, h_statx, h_readlinkat, h_getdents64;

static unsigned long layer1_strlen(const char *s) {
    unsigned long n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

static void layer1_memset(char *dst, int v, unsigned long n) {
    unsigned long i;
    if (!dst) return;
    for (i = 0; i < n; i++) dst[i] = (char)v;
}

static void layer1_memmove(char *dst, const char *src, unsigned long n) {
    unsigned long i;
    if (!dst || !src || n == 0 || dst == src) return;
    if (dst < src) {
        for (i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (i = n; i > 0; i--) dst[i - 1] = src[i - 1];
    }
}

static int layer1_contains(const char *haystack, const char *needle) {
    unsigned long hlen, nlen, i, j;
    if (!haystack || !needle || needle[0] == '\0') return 0;
    hlen = layer1_strlen(haystack);
    nlen = layer1_strlen(needle);
    if (nlen > hlen) return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int layer1_str_equal(const char *a, const char *b) {
    unsigned long i;
    if (!a || !b) return 0;
    for (i = 0;; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
}

static void layer1_copy_trunc(char *dst, const char *src, unsigned long dstsz) {
    unsigned long i;
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    for (i = 0; i + 1 < dstsz && src[i] != '\0'; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int layer1_is_root(void) {
    if (!kp_current_uid_fn) return 0;
    return kp_current_uid_fn() == 0;
}

static int layer1_should_hide(const char *path) {
    int i;
    if (!path || path[0] == '\0') return 0;
    for (i = 0; hide_list[i] != NULL; i++) {
        if (layer1_contains(path, hide_list[i])) return 1;
    }
    for (i = 0; i < extra_count; i++) {
        if (extra_hide[i][0] != '\0' && layer1_contains(path, extra_hide[i])) return 1;
    }
    return 0;
}

static void layer1_parse_hide_list_value(const char *val) {
    unsigned long i = 0, start, len;
    char tmp[L1_EXTRA_LEN];
    int j;
    if (!val) return;
    while (val[i] != '\0') {
        while (val[i] == ',' || val[i] == ' ' || val[i] == '\t') i++;
        if (val[i] == '\0') break;
        start = i;
        while (val[i] != '\0' && val[i] != ',') i++;
        len = i - start;
        if (len == 0) continue;
        while (len > 0 && (val[start + len - 1] == ' ' || val[start + len - 1] == '\t')) len--;
        if (len == 0) continue;
        if (len >= L1_EXTRA_LEN) len = L1_EXTRA_LEN - 1;
        for (j = 0; j < (int)len; j++) tmp[j] = val[start + j];
        tmp[len] = '\0';
        for (j = 0; j < extra_count; j++) {
            if (layer1_str_equal(extra_hide[j], tmp)) break;
        }
        if (j < extra_count) continue;
        if (extra_count >= L1_MAX_EXTRA) {
            pr_err("[layer1-hide] extra hide list full, ignoring: %s\n", tmp);
            return;
        }
        layer1_copy_trunc(extra_hide[extra_count], tmp, L1_EXTRA_LEN);
        extra_count++;
        pr_info("[layer1-hide] extra hide token added: %s\n", tmp);
    }
}

static int layer1_copy_path(const char __user *uptr, char *buf, unsigned long bufsz) {
    long rc;
    if (!uptr || !buf || bufsz == 0) return -EINVAL;
    buf[0] = '\0';
    rc = compat_strncpy_from_user(buf, (const void __user *)uptr, (long)bufsz - 1);
    if (rc <= 0) return -EFAULT;
    buf[bufsz - 1] = '\0';
    return 0;
}

static void before_openat(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_openat2(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_faccessat(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_faccessat2(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_newfstatat(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_statx(hook_fargs5_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

static void before_readlinkat(hook_fargs4_t *args, void *udata) {
    const char __user *uptr; char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root()) return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0) return;
    if (layer1_should_hide(buf)) { args->ret = -ENOENT; args->skip_origin = 1; }
}

struct layer1_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

static void after_getdents64(hook_fargs3_t *args, void *udata) {
    char tmp[L1_GETDENTS_BUF];
    void __user *dirp;
    long ret, src_off, dst_off;
    unsigned long name_off;
    (void)udata;
    if (!args || layer1_is_root()) return;
    ret = args->ret;
    if (ret <= 0) return;
    if (ret > (long)sizeof(tmp)) {
        if (!getdents_too_large_logged) {
            getdents_too_large_logged = 1;
            pr_info("[layer1-hide] getdents64 buffer too large, skipping\n");
        }
        return;
    }
    dirp = (void __user *)(unsigned long)syscall_argn(args, 1);
    if (!dirp || !kp_copy_from_user_fn || kp_copy_from_user_fn(tmp, dirp, (unsigned long)ret) != 0) return;
    
    name_off = (unsigned long)&((struct layer1_dirent64 *)0)->d_name;
    src_off = 0; dst_off = 0;
    
    while (src_off < ret) {
        struct layer1_dirent64 *d;
        unsigned long reclen, k;
        int has_nul;
        if (src_off + (long)name_off > ret) return;
        d = (struct layer1_dirent64 *)(tmp + src_off);
        reclen = (unsigned long)d->d_reclen;
        if (reclen < name_off + 1 || src_off + (long)reclen > ret) return;
        
        has_nul = 0;
        for (k = 0; k + name_off < reclen; k++) {
            if (d->d_name[k] == '\0') { has_nul = 1; break; }
        }
        if (!has_nul) return;
        
        if (layer1_should_hide(d->d_name)) {
            src_off += (long)reclen;
            continue;
        }
        if (dst_off != src_off) layer1_memmove(tmp + dst_off, tmp + src_off, reclen);
        dst_off += (long)reclen;
        src_off += (long)reclen;
    }
    
    if (dst_off != ret) {
        if (dst_off > 0 && kp_copy_to_user_fn) {
            kp_copy_to_user_fn(dirp, tmp, (unsigned long)dst_off);
        }
        args->ret = dst_off;
    }
}

static void layer1_uninstall_all(void) {
    if (h_getdents64) { unhook_syscalln(__NR_getdents64, 0, (void *)after_getdents64); h_getdents64 = 0; }
    if (h_readlinkat) { unhook_syscalln(__NR_readlinkat, (void *)before_readlinkat, 0); h_readlinkat = 0; }
    if (h_statx) { unhook_syscalln(__NR_statx, (void *)before_statx, 0); h_statx = 0; }
    if (h_newfstatat) { unhook_syscalln(__NR_newfstatat, (void *)before_newfstatat, 0); h_newfstatat = 0; }
    if (h_faccessat2) { unhook_syscalln(__NR_faccessat2, (void *)before_faccessat2, 0); h_faccessat2 = 0; }
    if (h_faccessat) { unhook_syscalln(__NR_faccessat, (void *)before_faccessat, 0); h_faccessat = 0; }
    if (h_openat2) { unhook_syscalln(__NR_openat2, (void *)before_openat2, 0); h_openat2 = 0; }
    if (h_openat) { unhook_syscalln(__NR_openat, (void *)before_openat, 0); h_openat = 0; }
}

static long layer1_install_all(void) {
    int rc;
    rc = (int)hook_syscalln(__NR_openat, 4, (void *)before_openat, 0, 0);
    if (rc != 0) return -EINVAL; h_openat = 1;

    rc = (int)hook_syscalln(__NR_openat2, 4, (void *)before_openat2, 0, 0);
    if (rc == 0) h_openat2 = 1;

    rc = (int)hook_syscalln(__NR_faccessat, 4, (void *)before_faccessat, 0, 0);
    if (rc != 0) return -EINVAL; h_faccessat = 1;

    rc = (int)hook_syscalln(__NR_faccessat2, 4, (void *)before_faccessat2, 0, 0);
    if (rc == 0) h_faccessat2 = 1;

    rc = (int)hook_syscalln(__NR_newfstatat, 4, (void *)before_newfstatat, 0, 0);
    if (rc != 0) return -EINVAL; h_newfstatat = 1;

    rc = (int)hook_syscalln(__NR_statx, 5, (void *)before_statx, 0, 0);
    if (rc != 0) return -EINVAL; h_statx = 1;

    rc = (int)hook_syscalln(__NR_readlinkat, 4, (void *)before_readlinkat, 0, 0);
    if (rc != 0) return -EINVAL; h_readlinkat = 1;

    rc = (int)hook_syscalln(__NR_getdents64, 3, 0, (void *)after_getdents64, 0);
    if (rc != 0) return -EINVAL; h_getdents64 = 1;

    return 0;
}

static long layer1_init(const char *args, const char *event, void *reserved) {
    long rc;
    (void)event; (void)reserved;

    kp_printk_fn = (int (*)(const char *, ...))kallsyms_lookup_name("printk");
    kp_current_uid_fn = (unsigned int (*)(void))kallsyms_lookup_name("current_uid");
    kp_copy_from_user_fn = (unsigned long (*)(void *, const void __user *, unsigned long))kallsyms_lookup_name("_copy_from_user");
    kp_copy_to_user_fn = (unsigned long (*)(void __user *, const void *, unsigned long))kallsyms_lookup_name("_copy_to_user");

    if (!kp_printk_fn || !kp_current_uid_fn || !kp_copy_from_user_fn || !kp_copy_to_user_fn) {
        return -1;
    }

    pr_info("[layer1-hide] init, version 1.0.0\n");
    extra_count = 0;
    getdents_too_large_logged = 0;

    if (args && args[0] != '\0') {
        // Parse args if needed
    }

    rc = layer1_install_all();
    if (rc != 0) {
        pr_err("[layer1-hide] install failed, rolling back\n");
        layer1_uninstall_all();
        return rc;
    }
    return 0;
}

static long layer1_exit(void *reserved) {
    (void)reserved;
    layer1_uninstall_all();
    pr_info("[layer1-hide] unloaded\n");
    return 0;
}

KPM_NAME("layer1-hide");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL-2.0");
KPM_AUTHOR("layer1-hide");
KPM_DESCRIPTION("Hide GameGuardian paths");
KPM_INIT(layer1_init);
KPM_EXIT(layer1_exit);
