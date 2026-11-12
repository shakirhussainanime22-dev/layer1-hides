/*
 * layer1-hide - Hide root / APatch / Magisk / Xposed / GameGuardian
 * filesystem paths from non-root processes.
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 * Target: bmax121/KernelPatch main branch, Android ARM64, GKI.
 */

#include <kpmodule.h>
#include <hook.h>
#include <syscall.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/types.h>
#include <linux/unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef __user
#define __user
#endif

/* ------------------------------------------------------------------ */
/* Hide list: edit here. One entry per line. Substring match.          */
/* ------------------------------------------------------------------ */

static const char *hide_list[] = {
    /* APatch / ADB paths */
    "/data/adb/",                  
    "/data/adb/modules",           
    "/data/adb/ap/",               
    "/data/adb/apd",              
    /* su binaries */
    "/system/bin/su",
    "/system/xbin/su",
    "/sbin/su",
    /* Generic tokens, match anywhere in path */
    "magisk",                      
    "Magisk",                      
    "xposed",                      
    "Xposed",                      
    "riru",                        
    "zygisk",                      
    "busybox",                     
    /* Magisk / ramdisk artifacts */
    "/sbin/.magisk/",
    "/debug_ramdisk/",
    /* GameGuardian */
    "/data/local/tmp/GG/",
    "/data/local/tmp/gameguardian/",
    "/sdcard/GameGuardian/",
    "/data/media/0/GameGuardian/",
    "/data/data/com.sand.air/",
    "/data/data/catch_.me_.if_.you_.can_/",
    "/data/data/com.hsjjolddvjeac/",       /* <--- YOUR GG PACKAGE */
    "/sdcard/Android/data/com.hsjjolddvjeac/", /* <--- YOUR GG SDCARD */
    NULL
};

/* ------------------------------------------------------------------ */
/* Mutable state: only this block plus hook-installed flags below.     */
/* No other global mutable state. No allocation in hot path.           */
/* ------------------------------------------------------------------ */

#define L1_MAX_EXTRA 16
#define L1_EXTRA_LEN 64
#define L1_GETDENTS_BUF 4096
#define L1_ARGS_BUF 512

static char extra_hide[L1_MAX_EXTRA][L1_EXTRA_LEN];
static int extra_count;
static int execve_hook_enabled; 
static int getdents_too_large_logged;

static int h_openat;
static int h_openat2;
static int h_faccessat;
static int h_faccessat2;
static int h_newfstatat;
static int h_statx;
static int h_readlinkat;
static int h_getdents64;
static int h_execve;

/* ------------------------------------------------------------------ */
/* Small string helpers. We implement our own so we do not depend on  */
/* kernel string.h exports that may not be visible to KPMs.           */
/* ------------------------------------------------------------------ */

static unsigned long layer1_strlen(const char *s)
{
    unsigned long n = 0;
    if (!s)
        return 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static void layer1_memset(char *dst, int v, unsigned long n)
{
    unsigned long i;
    if (!dst)
        return;
    for (i = 0; i < n; i++)
        dst[i] = (char)v;
}

static void layer1_memmove(char *dst, const char *src, unsigned long n)
{
    unsigned long i;
    if (!dst || !src || n == 0 || dst == src)
        return;
    if (dst < src) {
        for (i = 0; i < n; i++)
            dst[i] = src[i];
    } else {
        for (i = n; i > 0; i--)
            dst[i - 1] = src[i - 1];
    }
}

static int layer1_contains(const char *haystack, const char *needle)
{
    unsigned long hlen;
    unsigned long nlen;
    unsigned long i;
    unsigned long j;
    if (!haystack || !needle)
        return 0;
    if (needle[0] == '\0')
        return 0;
    hlen = layer1_strlen(haystack);
    nlen = layer1_strlen(needle);
    if (nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        j = 0;
        while (j < nlen && haystack[i + j] == needle[j])
            j++;
        if (j == nlen)
            return 1;
    }
    return 0;
}

static int layer1_str_equal(const char *a, const char *b)
{
    unsigned long i;
    if (!a || !b)
        return 0;
    for (i = 0;; i++) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == '\0')
            return 1;
    }
}

static void layer1_copy_trunc(char *dst, const char *src, unsigned long dstsz)
{
    unsigned long i;
    if (!dst || dstsz == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < dstsz && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void layer1_append_str(char *dst, unsigned long dstsz,
                              unsigned long *pos, const char *src)
{
    unsigned long i;
    if (!dst || !pos || !src || dstsz == 0)
        return;
    for (i = 0; src[i] != '\0' && *pos + 1 < dstsz; i++) {
        dst[*pos] = src[i];
        (*pos)++;
    }
    dst[*pos] = '\0';
}

static void layer1_append_int(char *dst, unsigned long dstsz,
                              unsigned long *pos, int v)
{
    char tmp[16];
    int len = 0;
    int neg = 0;
    int i;
    unsigned int u;
    if (!dst || !pos || dstsz == 0)
        return;
    if (v < 0) {
        neg = 1;
        u = (unsigned int)(-(v + 1)) + 1;
    } else {
        u = (unsigned int)v;
    }
    if (u == 0) {
        tmp[len++] = '0';
    } else {
        while (u > 0 && len < (int)sizeof(tmp)) {
            tmp[len++] = (char)('0' + (u % 10));
            u /= 10;
        }
    }
    if (neg && *pos + 1 < dstsz) {
        dst[*pos] = '-';
        (*pos)++;
    }
    for (i = len - 1; i >= 0; i--) {
        if (*pos + 1 >= dstsz)
            break;
        dst[*pos] = tmp[i];
        (*pos)++;
    }
    dst[*pos] = '\0';
}

static int layer1_is_root(void)
{
    return current_uid().val == 0;
}

static int layer1_should_hide(const char *path)
{
    int i;
    if (!path || path[0] == '\0')
        return 0;
    for (i = 0; hide_list[i] != NULL; i++) {
        if (layer1_contains(path, hide_list[i]))
            return 1;
    }
    for (i = 0; i < extra_count; i++) {
        if (extra_hide[i][0] != '\0' && layer1_contains(path, extra_hide[i]))
            return 1;
    }
    return 0;
}

static int layer1_basename_is_su(const char *path)
{
    const char *base;
    const char *p;
    if (!path || path[0] == '\0')
        return 0;
    base = path;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/')
            base = p + 1;
    }
    return base[0] == 's' && base[1] == 'u' && base[2] == '\0';
}

static int layer1_copy_path(const char __user *uptr, char *buf,
                            unsigned long bufsz)
{
    long rc;
    if (!uptr || !buf || bufsz == 0)
        return -EINVAL;
    buf[0] = '\0';
    rc = compat_strncpy_from_user(buf, (const void __user *)uptr,
                                  (long)bufsz - 1);
    if (rc <= 0)
        return -EFAULT;
    buf[bufsz - 1] = '\0';
    return 0;
}

static void layer1_parse_hide_list_value(const char *val)
{
    unsigned long i = 0;
    unsigned long start;
    unsigned long len;
    char tmp[L1_EXTRA_LEN];
    int j;
    if (!val)
        return;
    while (val[i] != '\0') {
        while (val[i] == ',' || val[i] == ' ' || val[i] == '\t')
            i++;
        if (val[i] == '\0')
            break;
        start = i;
        while (val[i] != '\0' && val[i] != ',')
            i++;
        len = i - start;
        if (len == 0)
            continue;
        while (len > 0 && (val[start + len - 1] == ' ' ||
                           val[start + len - 1] == '\t'))
            len--;
        if (len == 0)
            continue;
        if (len >= L1_EXTRA_LEN)
            len = L1_EXTRA_LEN - 1;
        for (j = 0; j < (int)len; j++)
            tmp[j] = val[start + j];
        tmp[len] = '\0';
        for (j = 0; j < extra_count; j++) {
            if (layer1_str_equal(extra_hide[j], tmp))
                break;
        }
        if (j < extra_count)
            continue;
        if (extra_count >= L1_MAX_EXTRA) {
            pr_err("[layer1-hide] extra hide list full, ignoring: %s\n", tmp);
            return;
        }
        layer1_copy_trunc(extra_hide[extra_count], tmp, L1_EXTRA_LEN);
        extra_count++;
        pr_info("[layer1-hide] extra hide token added: %s\n", tmp);
    }
}

static int layer1_extract_arg_value(const char *args, const char *key,
                                    char *out, unsigned long outsz)
{
    unsigned long klen;
    unsigned long i;
    unsigned long j;
    unsigned long alen;
    if (!args || !key || !out || outsz == 0)
        return -EINVAL;
    klen = layer1_strlen(key);
    alen = layer1_strlen(args);
    if (klen == 0 || alen < klen)
        return -EINVAL;
    for (i = 0; i + klen <= alen; i++) {
        unsigned long k;
        int match = 1;
        for (k = 0; k < klen; k++) {
            if (args[i + k] != key[k]) {
                match = 0;
                break;
            }
        }
        if (!match)
            continue;
        j = 0;
        i += klen;
        while (args[i] != '\0' && args[i] != ' ' && args[i] != '\t' &&
               args[i] != '"' && args[i] != '\'' && j + 1 < outsz) {
            out[j++] = args[i++];
        }
        out[j] = '\0';
        return 0;
    }
    return -EINVAL;
}

static void layer1_parse_args(const char *args)
{
    char val[L1_ARGS_BUF];
    if (!args || args[0] == '\0')
        return;
    pr_info("[layer1-hide] init args: %s\n", args);
    if (layer1_extract_arg_value(args, "hide_list=", val, sizeof(val)) == 0) {
        if (val[0] != '\0')
            layer1_parse_hide_list_value(val);
    }
    if (layer1_extract_arg_value(args, "execve_hook=", val, sizeof(val)) == 0) {
        if (val[0] == '1' || layer1_str_equal(val, "true") ||
            layer1_str_equal(val, "on")) {
            execve_hook_enabled = 1;
            pr_info("[layer1-hide] execve hook enabled via args\n");
        }
    }
}

static void before_openat(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

#ifdef __NR_openat2
static void before_openat2(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}
#endif

static void before_faccessat(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

#ifdef __NR_faccessat2
static void before_faccessat2(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}
#endif

static void before_newfstatat(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

static void before_statx(hook_fargs5_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

static void before_readlinkat(hook_fargs4_t *args, void *udata)
{
    const char __user *uptr;
    char buf[PATH_MAX];
    (void)udata;
    if (!args || layer1_is_root())
        return;
    uptr = (const char __user *)(unsigned long)syscall_argn(args, 1);
    if (!uptr || layer1_copy_path(uptr, buf, sizeof(buf)) != 0)
        return;
    if (layer1_should_hide(buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

static void before_execve(hook_fargs3_t *args, void *udata)
{
    const char __user *filename_ptr;
    const char __user * const __user *argv_ptr;
    const char __user *argv0_ptr;
    char buf[PATH_MAX];
    char argv0_buf[PATH_MAX];
    long rc;
    (void)udata;
    if (!args || layer1_is_root())
        return;
    filename_ptr = (const char __user *)(unsigned long)syscall_argn(args, 0);
    if (filename_ptr) {
        if (layer1_copy_path(filename_ptr, buf, sizeof(buf)) == 0) {
            if (layer1_basename_is_su(buf)) {
                args->ret = -ENOENT;
                args->skip_origin = 1;
                return;
            }
        }
    }
    argv_ptr = (const char __user * const __user *)(unsigned long)syscall_argn(args, 1);
    if (!argv_ptr)
        return;
    argv0_ptr = NULL;
    rc = compat_copy_from_user(&argv0_ptr, (const void __user *)argv_ptr,
                              sizeof(argv0_ptr));
    if (rc != 0 || !argv0_ptr)
        return;
    if (layer1_copy_path(argv0_ptr, argv0_buf, sizeof(argv0_buf)) != 0)
        return;
    if (layer1_basename_is_su(argv0_buf)) {
        args->ret = -ENOENT;
        args->skip_origin = 1;
    }
}

struct layer1_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

static void after_getdents64(hook_fargs3_t *args, void *udata)
{
    char tmp[L1_GETDENTS_BUF];
    void __user *dirp;
    long ret;
    long src_off;
    long dst_off;
    unsigned long name_off;
    (void)udata;
    if (!args || layer1_is_root())
        return;
    ret = args->ret;
    if (ret <= 0)
        return;
    if (ret > (long)sizeof(tmp)) {
        if (!getdents_too_large_logged) {
            getdents_too_large_logged = 1;
            pr_info("[layer1-hide] getdents64 buffer %ld > %lu, skipping filter\n",
                    ret, (unsigned long)sizeof(tmp));
        }
        return;
    }
    dirp = (void __user *)(unsigned long)syscall_argn(args, 1);
    if (!dirp || compat_copy_from_user(tmp, (const void __user *)dirp, (unsigned long)ret) != 0)
        return;
    name_off = (unsigned long)&((struct layer1_dirent64 *)0)->d_name;
    src_off = 0;
    dst_off = 0;
    while (src_off < ret) {
        struct layer1_dirent64 *d;
        unsigned long reclen;
        unsigned long k;
        int has_nul;
        if (src_off + (long)name_off > ret)
            return;
        d = (struct layer1_dirent64 *)(tmp + src_off);
        reclen = (unsigned long)d->d_reclen;
        if (reclen < name_off + 1 || src_off + (long)reclen > ret)
            return;
        has_nul = 0;
        for (k = 0; k + name_off < reclen; k++) {
            if (d->d_name[k] == '\0') {
                has_nul = 1;
                break;
            }
        }
        if (!has_nul)
            return;
        if (layer1_should_hide(d->d_name)) {
            src_off += (long)reclen;
            continue;
        }
        if (dst_off != src_off)
            layer1_memmove(tmp + dst_off, tmp + src_off, reclen);
        dst_off += (long)reclen;
        src_off += (long)reclen;
    }
    if (dst_off != ret) {
        if (dst_off > 0) {
            if (compat_copy_to_user(dirp, tmp, (unsigned long)dst_off) != 0)
                return;
        }
        args->ret = dst_off;
    }
}

static void layer1_uninstall_all(void)
{
    if (h_execve) { unhook_syscalln(__NR_execve, (void *)before_execve, 0); h_execve = 0; }
    if (h_getdents64) { unhook_syscalln(__NR_getdents64, 0, (void *)after_getdents64); h_getdents64 = 0; }
    if (h_readlinkat) { unhook_syscalln(__NR_readlinkat, (void *)before_readlinkat, 0); h_readlinkat = 0; }
    if (h_statx) { unhook_syscalln(__NR_statx, (void *)before_statx, 0); h_statx = 0; }
    if (h_newfstatat) { unhook_syscalln(__NR_newfstatat, (void *)before_newfstatat, 0); h_newfstatat = 0; }
#ifdef __NR_faccessat2
    if (h_faccessat2) { unhook_syscalln(__NR_faccessat2, (void *)before_faccessat2, 0); h_faccessat2 = 0; }
#endif
    if (h_faccessat) { unhook_syscalln(__NR_faccessat, (void *)before_faccessat, 0); h_faccessat = 0; }
#ifdef __NR_openat2
    if (h_openat2) { unhook_syscalln(__NR_openat2, (void *)before_openat2, 0); h_openat2 = 0; }
#endif
    if (h_openat) { unhook_syscalln(__NR_openat, (void *)before_openat, 0); h_openat = 0; }
}

static long layer1_install_all(void)
{
    int rc;
    rc = (int)hook_syscalln(__NR_openat, 4, (void *)before_openat, 0, 0);
    if (rc != 0) return -EINVAL;
    h_openat = 1;
#ifdef __NR_openat2
    rc = (int)hook_syscalln(__NR_openat2, 4, (void *)before_openat2, 0, 0);
    if (rc != 0) return -EINVAL;
    h_openat2 = 1;
#endif
    rc = (int)hook_syscalln(__NR_faccessat, 4, (void *)before_faccessat, 0, 0);
    if (rc != 0) return -EINVAL;
    h_faccessat = 1;
#ifdef __NR_faccessat2
    rc = (int)hook_syscalln(__NR_faccessat2, 4, (void *)before_faccessat2, 0, 0);
    if (rc != 0) return -EINVAL;
    h_faccessat2 = 1;
#endif
    rc = (int)hook_syscalln(__NR_newfstatat, 4, (void *)before_newfstatat, 0, 0);
    if (rc != 0) return -EINVAL;
    h_newfstatat = 1;
    rc = (int)hook_syscalln(__NR_statx, 5, (void *)before_statx, 0, 0);
    if (rc != 0) return -EINVAL;
    h_statx = 1;
    rc = (int)hook_syscalln(__NR_readlinkat, 4, (void *)before_readlinkat, 0, 0);
    if (rc != 0) return -EINVAL;
    h_readlinkat = 1;
    rc = (int)hook_syscalln(__NR_getdents64, 3, 0, (void *)after_getdents64, 0);
    if (rc != 0) return -EINVAL;
    h_getdents64 = 1;
    if (execve_hook_enabled) {
        rc = (int)hook_syscalln(__NR_execve, 3, (void *)before_execve, 0, 0);
        if (rc != 0) return -EINVAL;
        h_execve = 1;
    }
    return 0;
}

static long layer1_init(const char *args, const char *event, void *reserved)
{
    long rc;
    (void)event; (void)reserved;
    pr_info("[layer1-hide] init, version 1.0.0\n");
    extra_count = 0;
    execve_hook_enabled = 0;
    getdents_too_large_logged = 0;
    layer1_parse_args(args);
    rc = layer1_install_all();
    if (rc != 0) {
        pr_err("[layer1-hide] install failed, rolling back\n");
        layer1_uninstall_all();
        return rc;
    }
    return 0;
}

static long layer1_exit(void *reserved)
{
    (void)reserved;
    layer1_uninstall_all();
    pr_info("[layer1-hide] unloaded\n");
    return 0;
}

static long layer1_ctl0(const char *args, char __user *out_msg, int outlen)
{
    char out[256];
    unsigned long pos = 0;
    int i;
    if (!args) return 0;
    if (layer1_contains(args, "hide_list=")) {
        char val[L1_ARGS_BUF];
        if (layer1_extract_arg_value(args, "hide_list=", val, sizeof(val)) == 0)
            layer1_parse_hide_list_value(val);
    }
    if (layer1_contains(args, "execve_hook=1"))
        execve_hook_enabled = 1;
    if (layer1_contains(args, "status")) {
        layer1_memset(out, 0, sizeof(out));
        layer1_append_str(out, sizeof(out), &pos, "layer1-hide extra=");
        layer1_append_int(out, sizeof(out), &pos, extra_count);
        layer1_append_str(out, sizeof(out), &pos, " execve=");
        layer1_append_int(out, sizeof(out), &pos, execve_hook_enabled);
        layer1_append_str(out, sizeof(out), &pos, " hooks=");
        layer1_append_int(out, sizeof(out), &pos,
                          h_openat + h_openat2 + h_faccessat + h_faccessat2 +
                          h_newfstatat + h_statx + h_readlinkat +
                          h_getdents64 + h_execve);
        layer1_append_str(out, sizeof(out), &pos, "\n");
        if (out_msg && outlen > 0) {
            unsigned long copy_len = layer1_strlen(out);
            if (copy_len + 1 > (unsigned long)outlen)
                copy_len = (unsigned long)outlen - 1;
            if (compat_copy_to_user((void __user *)out_msg, out, copy_len) != 0)
                return -EFAULT;
        }
        for (i = 0; i < extra_count; i++)
            pr_info("[layer1-hide] extra[%d]=%s\n", i, extra_hide[i]);
    }
    return 0;
}

static long layer1_ctl1(void *a1, void *a2, void *a3)
{
    (void)a1; (void)a2; (void)a3;
    return 0;
}

KPM_NAME("layer1-hide");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL-2.0");
KPM_AUTHOR("layer1-hide");
KPM_DESCRIPTION("Hide root/APatch/Magisk/Xposed/GameGuardian paths from non-root processes");
KPM_INIT(layer1_init);
KPM_EXIT(layer1_exit);
KPM_CTL0(layer1_ctl0);
KPM_CTL1(layer1_ctl1);
