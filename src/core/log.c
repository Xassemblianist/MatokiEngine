/* core/log.c — mini printf implementasyonu ve log/panic.
 * Ayırma yapmaz; yığında sabit tampon kullanır (LOG satırı <= 1024 byte).
 * Float biçimlendirme basittir (|x| < 2^64 için tam, üstü "big"); hata ayıklama içindir.
 */
#include "core/log.h"
#include "plat/platform.h"

typedef struct {
    char* buf;
    u32   cap;  /* NUL için 1 byte ayrılmış kapasite */
    u32   len;
} FmtOut;

static void out_char(FmtOut* o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(FmtOut* o, char c, i32 n) {
    while (n-- > 0) out_char(o, c);
}

static u32 cstr_len(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

/* sayıyı tersten tmp'ye yazar, uzunluğu döndürür */
static u32 u64_digits(u64 v, u32 base, b32 upper, char* tmp) {
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    u32 n = 0;
    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    return n;
}

static void out_number(FmtOut* o, u64 v, b32 neg, u32 base, b32 upper,
                       i32 width, b32 zero, b32 left) {
    char tmp[24];
    u32 n = u64_digits(v, base, upper, tmp);
    i32 total = (i32)n + (neg ? 1 : 0);
    if (!left && !zero) out_pad(o, ' ', width - total);
    if (neg) out_char(o, '-');
    if (!left && zero) out_pad(o, '0', width - total);
    while (n) out_char(o, tmp[--n]);
    if (left) out_pad(o, ' ', width - total);
}

static void out_float(FmtOut* o, f64 x, i32 prec, i32 width, b32 zero, b32 left) {
    char tmp[64];
    u32 n = 0;
    if (x != x) { tmp[0] = 'n'; tmp[1] = 'a'; tmp[2] = 'n'; n = 3; goto emit_str; }
    b32 neg = x < 0.0 || (x == 0.0 && 1.0 / x < 0.0);
    if (neg) x = -x;
    if (x > 1.8e19) {
        const char* s = (x > 1.7976931348623157e308) ? "inf" : "big";
        if (neg) tmp[n++] = '-';
        for (u32 i = 0; i < 3; i++) tmp[n++] = s[i];
        goto emit_str;
    }
    if (prec > 9) prec = 9;
    {
        u64 scale = 1;
        for (i32 i = 0; i < prec; i++) scale *= 10;
        u64 ip = (u64)x;
        f64 frac = (x - (f64)ip) * (f64)scale + 0.5;
        u64 fp = (u64)frac;
        if (fp >= scale) { fp -= scale; ip++; }
        char d[24];
        u32 dn = u64_digits(ip, 10, FALSE, d);
        if (neg) tmp[n++] = '-';
        while (dn) tmp[n++] = d[--dn];
        if (prec > 0) {
            tmp[n++] = '.';
            for (i32 i = prec - 1; i >= 0; i--) { d[i] = (char)('0' + fp % 10); fp /= 10; }
            for (i32 i = 0; i < prec; i++) tmp[n++] = d[i];
        }
        /* sayı: '0' dolgusu işaretten sonra */
        i32 pad = width - (i32)n;
        if (!left && zero && pad > 0) {
            u32 start = 0;
            if (neg) { out_char(o, '-'); start = 1; }
            out_pad(o, '0', pad);
            for (u32 i = start; i < n; i++) out_char(o, tmp[i]);
            return;
        }
    }
emit_str:
    if (!left) out_pad(o, ' ', width - (i32)n);
    for (u32 i = 0; i < n; i++) out_char(o, tmp[i]);
    if (left) out_pad(o, ' ', width - (i32)n);
}

u32 fmt_vformat(char* buf, u32 cap, const char* fmt, VaList ap) {
    FmtOut o = { buf, cap, 0 };
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { out_char(&o, *p); continue; }
        p++;
        b32 left = FALSE, zero = FALSE;
        for (;; p++) {
            if (*p == '-') left = TRUE;
            else if (*p == '0') zero = TRUE;
            else break;
        }
        i32 width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        i32 prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        i32 lng = 0; /* 0: int, 1: long, 2: long long, 3: size */
        if (*p == 'l') { lng = 1; p++; if (*p == 'l') { lng = 2; p++; } }
        else if (*p == 'z') { lng = 3; p++; }

        switch (*p) {
        case 'd': case 'i': {
            i64 v;
            if (lng == 2) v = VA_ARG(ap, long long);
            else if (lng == 1) v = VA_ARG(ap, long);
            else if (lng == 3) v = (i64)VA_ARG(ap, isize);
            else v = VA_ARG(ap, int);
            b32 neg = v < 0;
            u64 u = neg ? (u64)0 - (u64)v : (u64)v;
            out_number(&o, u, neg, 10, FALSE, width, zero, left);
        } break;
        case 'u': case 'x': case 'X': {
            u64 v;
            if (lng == 2) v = VA_ARG(ap, unsigned long long);
            else if (lng == 1) v = VA_ARG(ap, unsigned long);
            else if (lng == 3) v = (u64)VA_ARG(ap, usize);
            else v = VA_ARG(ap, unsigned int);
            u32 base = (*p == 'u') ? 10 : 16;
            out_number(&o, v, FALSE, base, *p == 'X', width, zero, left);
        } break;
        case 'p': {
            uptr v = (uptr)VA_ARG(ap, void*);
            out_char(&o, '0'); out_char(&o, 'x');
            out_number(&o, (u64)v, FALSE, 16, FALSE, (i32)sizeof(void*) * 2, TRUE, FALSE);
        } break;
        case 'f': {
            f64 v = VA_ARG(ap, double);
            out_float(&o, v, prec < 0 ? 6 : prec, width, zero, left);
        } break;
        case 'c': {
            char c = (char)VA_ARG(ap, int);
            if (!left) out_pad(&o, ' ', width - 1);
            out_char(&o, c);
            if (left) out_pad(&o, ' ', width - 1);
        } break;
        case 's': {
            const char* s = VA_ARG(ap, const char*);
            if (!s) s = "(null)";
            u32 n = cstr_len(s);
            if (prec >= 0 && (u32)prec < n) n = (u32)prec;
            if (!left) out_pad(&o, ' ', width - (i32)n);
            for (u32 i = 0; i < n; i++) out_char(&o, s[i]);
            if (left) out_pad(&o, ' ', width - (i32)n);
        } break;
        case '%': out_char(&o, '%'); break;
        case '\0': p--; break;
        default: out_char(&o, '%'); out_char(&o, *p); break;
        }
    }
    if (cap > 0) buf[o.len < cap ? o.len : cap - 1] = '\0';
    return o.len < cap ? o.len : (cap ? cap - 1 : 0);
}

u32 fmt_format(char* buf, u32 cap, const char* fmt, ...) {
    VaList ap;
    VA_START(ap, fmt);
    u32 n = fmt_vformat(buf, cap, fmt, ap);
    VA_END(ap);
    return n;
}

void log_printf(const char* fmt, ...) {
    char buf[1024];
    VaList ap;
    VA_START(ap, fmt);
    u32 n = fmt_vformat(buf, sizeof(buf), fmt, ap);
    VA_END(ap);
    plat_log(buf, n);
}

_Noreturn void log_panic(const char* file, i32 line, const char* fmt, ...) {
    char buf[1024];
    u32 n = fmt_format(buf, sizeof(buf), "%s:%d: ", file, line);
    VaList ap;
    VA_START(ap, fmt);
    n += fmt_vformat(buf + n, (u32)sizeof(buf) - n, fmt, ap);
    VA_END(ap);
    plat_panic(buf, n);
    __builtin_trap();
}
