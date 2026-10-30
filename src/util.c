#include "util.h"
#include <time.h>

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("Error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal("out of memory"); return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) fatal("out of memory"); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) fatal("out of memory"); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *r = xmalloc(n + 1); memcpy(r, s, n); r[n] = '\0'; return r; }

/* ---- string builder ---- */

void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        b->s = xrealloc(b->s, cap);
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}
void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bputc(Buf *b, char c) { bput(b, &c, 1); }

void bprintf(Buf *b, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(small)) { bput(b, small, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    bput(b, big, (size_t)n);
    free(big);
}

void bjson_str(Buf *b, const char *s, size_t n) {
    bputc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { bputc(b, '\\'); bputc(b, (char)c); }
        else if (c == '\n') bputs(b, "\\n");
        else if (c == '\t') bputs(b, "\\t");
        else if (c == '\r') bputs(b, "\\r");
        else if (c < 32 || c == 127) bprintf(b, "\\u%04x", c);
        else if (c >= 128) {
            /* pass valid UTF-8 through; escape stray bytes so the JSON stays valid */
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
            int ok = len && i + (size_t)len <= n;
            for (int k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
            if (ok) { bput(b, s + i, (size_t)len); i += (size_t)len - 1; }
            else bprintf(b, "\\u%04x", c);
        }
        else bputc(b, (char)c);
    }
    bputc(b, '"');
}
void bjson_cstr(Buf *b, const char *s) { if (s) bjson_str(b, s, strlen(s)); else bputs(b, "null"); }

char *bdetach(Buf *b) {
    if (!b->s) bput(b, "", 0);
    char *s = b->s;
    b->s = NULL; b->n = b->cap = 0;
    return s;
}

/* ---- interning: open-addressing hash set of strings ---- */

static const char **itab;
static size_t icap, icount;

static size_t hash_str(const char *s, size_t n) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

const char *intern_n(const char *s, size_t n) {
    if (icount * 2 >= icap) {
        size_t ncap = icap ? icap * 2 : 1024;
        const char **nt = xcalloc(ncap, sizeof(char *));
        for (size_t i = 0; i < icap; i++) {
            if (!itab[i]) continue;
            size_t h = hash_str(itab[i], strlen(itab[i])) & (ncap - 1);
            while (nt[h]) h = (h + 1) & (ncap - 1);
            nt[h] = itab[i];
        }
        free(itab);
        itab = nt; icap = ncap;
    }
    size_t h = hash_str(s, n) & (icap - 1);
    while (itab[h]) {
        if (strlen(itab[h]) == n && memcmp(itab[h], s, n) == 0) return itab[h];
        h = (h + 1) & (icap - 1);
    }
    itab[h] = xstrndup(s, n);
    icount++;
    return itab[h];
}
const char *intern(const char *s) { return intern_n(s, strlen(s)); }

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

char *read_file(const char *path, long *len_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, n = 0, got;
    char *buf = xmalloc(cap);
    while ((got = fread(buf + n, 1, cap - n - 1, fp)) > 0) {
        n += got;
        if (n + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); }
    }
    int err = ferror(fp);
    fclose(fp);
    if (err) { free(buf); return NULL; }
    buf[n] = '\0';
    if (len_out) *len_out = (long)n;
    return buf;
}

/* ---- diagnostics ---- */

void diag(Diags *ds, Severity sev, const char *stage, const char *category,
          int line, int col, const char *hint, const char *fmt, ...) {
    if (sev == SEV_ERROR) ds->errors++;
    else if (sev == SEV_WARNING) ds->warnings++;
    if (ds->limit && ds->d.n >= ds->limit) return;   /* counted, but not stored */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Diag d = { sev, stage, category, line, col, xstrdup(msg), hint ? xstrdup(hint) : NULL };
    vpush(ds->d, d);
}

int diag_count_stage(const Diags *ds, const char *stage, Severity sev) {
    int n = 0;
    for (int i = 0; i < ds->d.n; i++)
        if (ds->d.v[i].sev == sev && strcmp(ds->d.v[i].stage, stage) == 0) n++;
    return n;
}
