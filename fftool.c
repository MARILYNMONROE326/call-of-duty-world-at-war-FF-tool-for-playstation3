/*
 * FFTool - drag & drop unpacker / repacker for PS3 Call of Duty: World at War
 * fastfiles (IWffu100, chunked raw-deflate, 64 KB blocks).
 *
 *   Drag  NAME.ff            -> NAME_unpacked\  (NAME.zone, ffinfo.bin, rawfiles\...)
 *   Drag  NAME_unpacked      -> NAME_rebuilt.ff (byte-identical if nothing was edited)
 *   Drag  NAME.zone          -> same as dragging its folder
 *
 * Edited GSC/CSC/other rawfiles in rawfiles\ are injected on rebuild. An edited
 * file may be shorter or equal to the original (it is padded with spaces), but
 * never longer, because growing an asset would shift every pointer after it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "zlib.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define SEP '/'
#endif

#define BLOCK 0x10000
#define TOOL_VER "1.4"

/* ------------------------------------------------------------------ */
/* UTF-8 aware file helpers                                            */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static FILE *u8fopen(const char *p, const char *m) {
    wchar_t *wp = widen(p), *wm = widen(m);
    FILE *f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
}
static int u8isdir(const char *p) {
    wchar_t *wp = widen(p);
    DWORD a = GetFileAttributesW(wp);
    free(wp);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int u8exists(const char *p) {
    wchar_t *wp = widen(p);
    DWORD a = GetFileAttributesW(wp);
    free(wp);
    return a != INVALID_FILE_ATTRIBUTES;
}
static int u8mkdir(const char *p) {
    wchar_t *wp = widen(p);
    int r = _wmkdir(wp);
    free(wp);
    return r;
}
#else
static FILE *u8fopen(const char *p, const char *m) { return fopen(p, m); }
static int u8isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int u8exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int u8mkdir(const char *p) { return mkdir(p, 0755); }
#endif

static int g_errors = 0;
static void err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stdout, "  ERROR: ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
    g_errors++;
}

static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *r = malloc(n); memcpy(r, s, n); return r; }

static char *join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    size_t o = la;
    if (la && a[la - 1] != '\\' && a[la - 1] != '/') r[o++] = SEP;
    memcpy(r + o, b, lb + 1);
    return r;
}

/* mkdir -p for every directory component of a file path */
static void make_parents(const char *path) {
    char *p = xstrdup(path);
    for (char *c = p + 1; *c; c++) {
        if (*c == '\\' || *c == '/') {
            char save = *c; *c = 0;
            if (!(c > p && c[-1] == ':') && !u8exists(p)) u8mkdir(p);
            *c = save;
        }
    }
    free(p);
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = u8fopen(path, "rb");
    if (!f) return NULL;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    long long n = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    unsigned char *b = malloc(n ? (size_t)n : 1);
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int write_file(const char *path, const void *data, size_t len) {
    make_parents(path);
    FILE *f = u8fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f)) ok = 0;
    return ok;
}

static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wbe32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* path helpers */
static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *c = p; *c; c++) if (*c == '\\' || *c == '/') b = c + 1;
    return b;
}
static char *dirname_of(const char *p) {
    char *r = xstrdup(p);
    char *b = (char *)basename_of(r);
    if (b > r) b[-1] = 0; else strcpy(r, ".");
    return r;
}
static char *strip_ext(const char *name) {
    char *r = xstrdup(name);
    char *dot = strrchr(r, '.');
    if (dot && dot != r) *dot = 0;
    return r;
}
static int ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
static void strip_trailing_sep(char *p) {
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '\\' || p[n - 1] == '/') && !(n == 3 && p[1] == ':')) p[--n] = 0;
}

/* ------------------------------------------------------------------ */
/* Compression of one 64 KB block exactly like the original linker     */
/* ------------------------------------------------------------------ */
static unsigned char g_cbuf[BLOCK * 2];

/* returns compressed length (raw deflate, without adler) or -1 */
static long compress_block(const unsigned char *in, size_t n) {
    z_stream s; memset(&s, 0, sizeof s);
    if (deflateInit2(&s, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    s.next_in = (Bytef *)in; s.avail_in = (uInt)n;
    s.next_out = g_cbuf; s.avail_out = sizeof g_cbuf;
    int r = deflate(&s, Z_FINISH);
    long out = (long)(sizeof g_cbuf - s.avail_out);
    deflateEnd(&s);
    return r == Z_STREAM_END ? out : -1;
}

/* ------------------------------------------------------------------ */
/* Rawfile scanning                                                    */
/* ------------------------------------------------------------------ */
typedef struct { size_t data_off; uint32_t len; char *name; char *relpath; } RawFile;

static int name_char_ok(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '/' || c == '\\' || c == '.' || c == '-' || c == '@' || c == '#' || c == '+' || c == '&';
}

static RawFile *scan_rawfiles(const unsigned char *z, size_t zl, int *count) {
    int cap = 64, n = 0;
    RawFile *rf = malloc(cap * sizeof *rf);
    for (size_t i = 0; i + 13 < zl; i++) {
        if (z[i] != 0xFF || be32(z + i) != 0xFFFFFFFFu || be32(z + i + 8) != 0xFFFFFFFFu) continue;
        uint32_t len = be32(z + i + 4);
        if (len == 0 || len > 0x4000000) continue;
        size_t ns = i + 12, ne = ns;
        int dot = 0;
        while (ne < zl && ne - ns < 256 && name_char_ok(z[ne])) { if (z[ne] == '.') dot = 1; ne++; }
        if (ne >= zl || z[ne] != 0 || ne - ns < 3 || !dot) continue;
        size_t ds = ne + 1;
        if (ds + len >= zl || z[ds + len] != 0) continue;
        /* reject names with '..' segments or leading separators */
        char *name = malloc(ne - ns + 1);
        memcpy(name, z + ns, ne - ns); name[ne - ns] = 0;
        if (strstr(name, "..") || name[0] == '/' || name[0] == '\\' || name[0] == '.') { free(name); continue; }
        if (n == cap) { cap *= 2; rf = realloc(rf, cap * sizeof *rf); }
        rf[n].data_off = ds; rf[n].len = len; rf[n].name = name; rf[n].relpath = NULL;
        n++;
        i = ds + len; /* skip past data */
    }
    /* unique relative paths */
    for (int k = 0; k < n; k++) {
        char *p = xstrdup(rf[k].name);
        for (char *c = p; *c; c++) if (*c == '/' || *c == '\\') *c = SEP;
        int dup = 0;
        for (int j = 0; j < k; j++) if (!strcmp(rf[j].name, rf[k].name)) dup++;
        if (dup) {
            char *q = malloc(strlen(p) + 16);
            sprintf(q, "%s.dup%d", p, dup);
            free(p); p = q;
        }
        rf[k].relpath = p;
    }
    *count = n;
    return rf;
}

#include "xmodel.c"
#include "xtexture.c"


/* ------------------------------------------------------------------ */
/* GSC compactor: strips comments, indentation, blank lines and extra  */
/* spaces (never inside strings). The game compiles GSC from text, so  */
/* behaviour is unchanged - it only makes room for bigger edits.       */
/* ------------------------------------------------------------------ */
static size_t gsc_compact(const unsigned char *in, size_t n, unsigned char *out) {
    /* pass 1: remove comments, keep newlines */
    unsigned char *t = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = in[i];
        if (c == '"') {                       /* string literal */
            t[k++] = in[i++];
            while (i < n && in[i] != '"' && in[i] != '\n') {
                if (in[i] == '\\' && i + 1 < n) t[k++] = in[i++];
                t[k++] = in[i++];
            }
            if (i < n && in[i] == '"') t[k++] = in[i++];
        } else if (c == '/' && i + 1 < n && in[i + 1] == '/') {
            while (i < n && in[i] != '\n') i++;
        } else if (c == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) { if (in[i] == '\n') t[k++] = '\n'; i++; }
            i = i + 2 <= n ? i + 2 : n;
            t[k++] = ' ';
        } else t[k++] = in[i++];
    }
    /* pass 2: per line trim, collapse whitespace outside strings, drop empty lines */
    size_t o = 0;
    for (size_t i = 0; i < k;) {
        size_t e = i;
        while (e < k && t[e] != '\n') e++;
        size_t ls = o;
        int instr = 0, pend = 0;
        for (size_t j = i; j < e; j++) {
            unsigned char c = t[j];
            if (instr) {
                out[o++] = c;
                if (c == '\\' && j + 1 < e) { out[o++] = t[++j]; continue; }
                if (c == '"') instr = 0;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') { if (o > ls) pend = 1; continue; }
            if (pend) { out[o++] = ' '; pend = 0; }
            out[o++] = c;
            if (c == '"') instr = 1;
        }
        if (o > ls) out[o++] = '\n';
        i = e + 1;
    }
    free(t);
    return o;
}

/* ------------------------------------------------------------------ */
/* UNPACK                                                              */
/* ------------------------------------------------------------------ */
static void do_unpack(const char *ffpath) {
    printf("\n== Unpacking %s\n", basename_of(ffpath));
    size_t fl;
    unsigned char *ff = read_file(ffpath, &fl);
    if (!ff) { err("cannot read file"); return; }
    if (fl < 16 || memcmp(ff, "IWff", 4)) { err("not a fastfile (missing IWff header)"); free(ff); return; }

    const size_t HDR = 12;
    printf("  Header : %.8s  version 0x%X\n", ff, be32(ff + 8));

    size_t zcap = 64u * 1024 * 1024, zl = 0;
    unsigned char *zone = malloc(zcap);
    size_t pos = HDR;
    int blocks = 0, mismatch = 0;
    unsigned char out[BLOCK];
    while (pos + 2 <= fl) {
        unsigned L = (ff[pos] << 8) | ff[pos + 1];
        if (L < 6 || pos + 2 + L > fl) break; /* end marker / trailer */
        const unsigned char *blk = ff + pos + 2;
        z_stream s; memset(&s, 0, sizeof s);
        inflateInit2(&s, -15);
        s.next_in = (Bytef *)blk; s.avail_in = L - 4;
        s.next_out = out; s.avail_out = BLOCK;
        int r = inflate(&s, Z_FINISH);
        size_t got = BLOCK - s.avail_out;
        int consumed_all = s.avail_in == 0;
        inflateEnd(&s);
        if (r != Z_STREAM_END || !consumed_all) {
            if (blocks == 0) { err("this fastfile is not in the PS3 chunked format this tool supports"); free(ff); free(zone); return; }
            break;
        }
        if (adler32(1, out, (uInt)got) != be32(blk + L - 4)) { err("checksum mismatch in block %d", blocks); free(ff); free(zone); return; }
        long cl = compress_block(out, got);
        if (cl != (long)(L - 4) || memcmp(g_cbuf, blk, L - 4)) mismatch++;
        if (zl + got > zcap) { zcap *= 2; zone = realloc(zone, zcap); }
        memcpy(zone + zl, out, got); zl += got;
        pos += 2 + L; blocks++;
        if ((blocks & 255) == 0) { printf("\r  Blocks : %d", blocks); fflush(stdout); }
    }
    size_t trailer = fl - pos;
    printf("\r  Blocks : %d  (zone %zu bytes, trailer %zu bytes)\n", blocks, zl, trailer);

    /* output folder */
    char *dir = dirname_of(ffpath);
    char *stem = strip_ext(basename_of(ffpath));
    char *sub = malloc(strlen(stem) + 16); sprintf(sub, "%s_unpacked", stem);
    char *outdir = join(dir, sub);
    if (u8exists(outdir)) {
        err("folder already exists: %s\n         Delete or rename it first (so your edits are never overwritten).", outdir);
        goto done;
    }
    u8mkdir(outdir);

    /* zone */
    char *zname = malloc(strlen(stem) + 8); sprintf(zname, "%s.zone", stem);
    char *zpath = join(outdir, zname);
    if (!write_file(zpath, zone, zl)) { err("cannot write %s", zpath); goto done; }

    /* ffinfo.bin : "FFTOOL01" | hdrlen | hdr | trailerlen | trailer | zonename */
    {
        size_t il = 8 + 4 + HDR + 4 + trailer + strlen(zname);
        unsigned char *info = malloc(il), *p = info;
        memcpy(p, "FFTOOL01", 8); p += 8;
        wbe32(p, (uint32_t)HDR); p += 4; memcpy(p, ff, HDR); p += HDR;
        wbe32(p, (uint32_t)trailer); p += 4; memcpy(p, ff + pos, trailer); p += trailer;
        memcpy(p, zname, strlen(zname));
        char *ip = join(outdir, "ffinfo.bin");
        if (!write_file(ip, info, il)) err("cannot write ffinfo.bin");
        free(info); free(ip);
    }

    /* rawfiles */
    int nraw = 0;
    RawFile *rf = scan_rawfiles(zone, zl, &nraw);
    char *rawdir = join(outdir, "rawfiles");
    u8mkdir(rawdir);
    char *idxp = join(rawdir, "_index.txt");
    FILE *idx = u8fopen(idxp, "wb");
    if (idx) {
        fprintf(idx, "# FFTool rawfile index - do not edit. offset<TAB>length<TAB>path\r\n");
        for (int k = 0; k < nraw; k++) {
            char *fp = join(rawdir, rf[k].relpath);
            if (!write_file(fp, zone + rf[k].data_off, rf[k].len)) err("cannot write %s", fp);
            fprintf(idx, "%zu\t%u\t%s\r\n", rf[k].data_off, rf[k].len, rf[k].relpath);
            free(fp);
        }
        fclose(idx);
    }
    printf("  Rawfiles extracted : %d (scripts etc. in rawfiles\\)\n", nraw);
    printf("  Exporting models...\r"); fflush(stdout);
    int nmod = models_export(zone, zl, outdir);
    printf("  Models exported    : %d (.obj files in models\\)\n", nmod);
    printf("  Exporting textures...\r"); fflush(stdout);
    int ntex = textures_export(zone, zl, outdir);
    printf("  Textures exported  : %d (.dds files in textures\\)\n", ntex);
    printf("  Output folder      : %s\n", outdir);
    if (mismatch)
        printf("  WARNING: %d block(s) could not be reproduced exactly; a rebuild will still load but won't be byte-identical.\n", mismatch);
    else
        printf("  Self-test          : all %d blocks recompress byte-for-byte -> rebuild will be identical.\n", blocks);
done:
    free(ff); free(zone); free(dir); free(stem); free(sub); free(outdir);
}

/* ------------------------------------------------------------------ */
/* REPACK                                                              */
/* ------------------------------------------------------------------ */
static void do_repack(const char *folder) {
    char *dirp = xstrdup(folder);
    strip_trailing_sep(dirp);
    printf("\n== Rebuilding from %s\n", basename_of(dirp));

    char *ip = join(dirp, "ffinfo.bin");
    size_t il;
    unsigned char *info = read_file(ip, &il);
    if (!info || il < 20 || memcmp(info, "FFTOOL01", 8)) { err("ffinfo.bin missing or invalid in this folder (unpack the .ff with this tool first)"); free(ip); free(dirp); free(info); return; }
    unsigned char *p = info + 8;
    uint32_t hl = be32(p); p += 4; unsigned char *hdr = p; p += hl;
    uint32_t tl = be32(p); p += 4; unsigned char *trl = p; p += tl;
    size_t znl = il - (size_t)(p - info);
    char *zname = malloc(znl + 1); memcpy(zname, p, znl); zname[znl] = 0;

    char *zpath = join(dirp, zname);
    size_t zl;
    unsigned char *zone = read_file(zpath, &zl);
    if (!zone) { err("cannot read %s", zpath); goto out0; }

    /* inject edited rawfiles */
    {
        char *rawdir = join(dirp, "rawfiles");
        char *idxp = join(rawdir, "_index.txt");
        size_t xl;
        char *idx = (char *)read_file(idxp, &xl);
        int changed = 0, toobig = 0;
        if (idx) {
            idx = realloc(idx, xl + 1); idx[xl] = 0;
            char *line = idx;
            while (line && *line) {
                char *nl = strpbrk(line, "\r\n");
                if (nl) { *nl = 0; nl++; while (*nl == '\r' || *nl == '\n') nl++; }
                if (*line && *line != '#') {
                    unsigned long long off; unsigned len; char rel[1024];
                    if (sscanf(line, "%llu\t%u\t%1023[^\r\n]", &off, &len, rel) == 3 && off + len < zl) {
                        char *fp = join(rawdir, rel);
                        size_t nl2;
                        unsigned char *data = read_file(fp, &nl2);
                        if (!data) printf("  (missing, kept original) %s\n", rel);
                        else if (nl2 != len || memcmp(data, zone + off, len)) {
                            int compacted = 0;
                            if (nl2 > len) {
                                unsigned char *cbuf = malloc(nl2 + 1);
                                size_t cl = gsc_compact(data, nl2, cbuf);
                                if (cl <= len) {
                                    free(data); data = cbuf;
                                    printf("  %s was %zu bytes too big - auto-compacted (comments/indentation removed) to %zu bytes\n", rel, nl2 - len, cl);
                                    nl2 = cl; compacted = 1;
                                } else free(cbuf);
                            }
                            if (nl2 > len) {
                                err("%s is %zu bytes too big (max %u bytes), even after removing comments and indentation. Shorten it.", rel, nl2 - len, len);
                                toobig++;
                            } else {
                                (void)compacted;
                                memcpy(zone + off, data, nl2);
                                memset(zone + off + nl2, ' ', len - nl2);
                                printf("  Injected %s (%zu/%u bytes%s)\n", rel, nl2, len, nl2 < len ? ", padded" : "");
                                changed++;
                            }
                        }
                        free(data); free(fp);
                    }
                }
                line = nl;
            }
            free(idx);
        }
        free(rawdir); free(idxp);
        if (toobig) { printf("  Nothing was written because of the error(s) above.\n"); goto out0; }
        int mc = models_import(zone, zl, dirp);
        if (mc < 0) { printf("  Nothing was written because of the error(s) above.\n"); goto out0; }
        int tc = textures_import(zone, zl, dirp);
        if (tc < 0) { printf("  Nothing was written because of the error(s) above.\n"); goto out0; }
        if (!changed && !mc && !tc) printf("  No edited scripts, models or textures - rebuilding original data.\n");
    }

    /* output path: sibling of folder */
    char *parent = dirname_of(dirp);
    char *fname = xstrdup(basename_of(dirp));
    size_t fnl = strlen(fname);
    if (fnl > 9 && ends_with_ci(fname, "_unpacked")) fname[fnl - 9] = 0;
    char *outname = malloc(strlen(fname) + 16); sprintf(outname, "%s_rebuilt.ff", fname);
    char *outp = join(parent, outname);
    char *origname = malloc(strlen(fname) + 8); sprintf(origname, "%s.ff", fname);
    char *origp = join(parent, origname);

    FILE *f = u8fopen(outp, "wb");
    if (!f) { err("cannot create %s", outp); goto out1; }
    fwrite(hdr, 1, hl, f);
    int blocks = 0;
    for (size_t off = 0; off < zl; off += BLOCK) {
        size_t n = zl - off < BLOCK ? zl - off : BLOCK;
        long cl = compress_block(zone + off, n);
        if (cl < 0 || cl + 4 > 0xFFFF) { err("block %d does not fit (compressed %ld bytes)", blocks, cl); fclose(f); goto out1; }
        unsigned char lb[2] = { (unsigned char)((cl + 4) >> 8), (unsigned char)(cl + 4) };
        unsigned char ad[4]; wbe32(ad, adler32(1, zone + off, (uInt)n));
        fwrite(lb, 1, 2, f); fwrite(g_cbuf, 1, cl, f); fwrite(ad, 1, 4, f);
        blocks++;
        if ((blocks & 255) == 0) { printf("\r  Blocks : %d", blocks); fflush(stdout); }
    }
    fwrite(trl, 1, tl, f);
    if (fclose(f)) { err("write failed for %s", outp); goto out1; }
    printf("\r  Blocks : %d\n", blocks);
    printf("  Wrote  : %s\n", outp);

    /* compare with original if present */
    size_t a, b;
    unsigned char *orig = read_file(origp, &a);
    if (orig) {
        unsigned char *neu = read_file(outp, &b);
        if (neu && a == b && !memcmp(orig, neu, a)) printf("  Check  : IDENTICAL to %s (byte-for-byte, %zu bytes)\n", origname, a);
        else if (neu) {
            size_t i = 0, m = a < b ? a : b;
            while (i < m && orig[i] == neu[i]) i++;
            printf("  Check  : differs from %s (first difference at 0x%zX) - expected if you edited files.\n", origname, i);
        }
        free(neu); free(orig);
    }
out1:
    free(parent); free(fname); free(outname); free(outp); free(origname); free(origp);
out0:
    free(zone); free(zpath); free(zname); free(info); free(ip); free(dirp);
}

/* ================================================================== */
/* PAK  (PS3 sound archive, magic "0KAP", big-endian)                  */
/*   0x00 magic  0x04 id  0x08 count  0x0C align  0x10 data start      */
/*   0x14 table: count x { u32 hash, u32 sector, u32 size }            */
/*   file i lives at datastart + sector*align                          */
/* ================================================================== */
#ifdef _WIN32
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif

#define PAK_HDR 0x14

typedef struct { uint32_t hash, sector, size; uint64_t start; int idx; } PakEnt;

static int cmp_start(const void *a, const void *b) {
    const PakEnt *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : x->idx - y->idx;
}

static uint64_t file_size64(const char *path, int *ok) {
    FILE *f = u8fopen(path, "rb");
    if (!f) { *ok = 0; return 0; }
    FSEEK64(f, 0, SEEK_END);
    uint64_t n = (uint64_t)FTELL64(f);
    fclose(f); *ok = 1;
    return n;
}

static const char *guess_ext(const unsigned char *d, size_t n) {
    if (n >= 3 && !memcmp(d, "ID3", 3)) return "mp3";
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "mp3";
    if (n >= 3 && !memcmp(d, "MSF", 3)) return "msf";
    if (n >= 4 && !memcmp(d, "RIFF", 4)) return "wav";
    if (n >= 4 && !memcmp(d, "OggS", 4)) return "ogg";
    if (n >= 4 && !memcmp(d, "VAGp", 4)) return "vag";
    return "bin";
}

static int fwrite_u32(FILE *f, uint32_t v) { unsigned char b[4]; wbe32(b, v); return fwrite(b, 1, 4, f) == 4; }
static int fwrite_u64(FILE *f, uint64_t v) { return fwrite_u32(f, (uint32_t)(v >> 32)) && fwrite_u32(f, (uint32_t)v); }

/* copy len bytes from in (current pos) to out; returns 1 on success */
static int copy_bytes(FILE *in, FILE *out, uint64_t len, unsigned char *buf, size_t bufsz) {
    while (len) {
        size_t c = len < bufsz ? (size_t)len : bufsz;
        if (fread(buf, 1, c, in) != c) return 0;
        if (out && fwrite(buf, 1, c, out) != c) return 0;
        len -= c;
    }
    return 1;
}

static int compare_files(const char *a, const char *b, uint64_t *diff_at) {
    FILE *fa = u8fopen(a, "rb"), *fb = u8fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return -1; }
    static unsigned char ba[1 << 20], bb[1 << 20];
    uint64_t pos = 0; int same = 1;
    for (;;) {
        size_t ra = fread(ba, 1, sizeof ba, fa), rb = fread(bb, 1, sizeof bb, fb);
        size_t m = ra < rb ? ra : rb;
        if (memcmp(ba, bb, m) || ra != rb) {
            size_t i = 0; while (i < m && ba[i] == bb[i]) i++;
            *diff_at = pos + i; same = 0; break;
        }
        if (!ra) break;
        pos += ra;
    }
    fclose(fa); fclose(fb);
    return same;
}

static void do_pak_unpack(const char *path) {
    printf("\n== Unpacking %s\n", basename_of(path));
    FILE *f = u8fopen(path, "rb");
    if (!f) { err("cannot open file"); return; }
    FSEEK64(f, 0, SEEK_END);
    uint64_t fsize = (uint64_t)FTELL64(f);
    FSEEK64(f, 0, SEEK_SET);
    unsigned char hdr[PAK_HDR];
    if (fread(hdr, 1, PAK_HDR, f) != PAK_HDR) { err("file too small"); fclose(f); return; }
    uint32_t n = be32(hdr + 8), align = be32(hdr + 12), dstart = be32(hdr + 16);
    printf("  Header : 0KAP  id 0x%08X  %u files  align %u  data @0x%X\n", be32(hdr + 4), n, align, dstart);
    if (!n || n > 10000000 || !align || (align & (align - 1)) || (uint64_t)PAK_HDR + 12ull * n > dstart || dstart > fsize) {
        err("unsupported or corrupt .pak header"); fclose(f); return;
    }
    size_t tl = 12u * n;
    unsigned char *tbl = malloc(tl);
    if (fread(tbl, 1, tl, f) != tl) { err("cannot read table"); free(tbl); fclose(f); return; }
    PakEnt *e = malloc(sizeof(PakEnt) * n);
    uint32_t maxsize = 0;
    for (uint32_t i = 0; i < n; i++) {
        e[i].hash = be32(tbl + 12 * i); e[i].sector = be32(tbl + 12 * i + 4); e[i].size = be32(tbl + 12 * i + 8);
        e[i].start = dstart + (uint64_t)e[i].sector * align; e[i].idx = (int)i;
        if (e[i].start + e[i].size > fsize) { err("entry %u points outside the file", i); free(tbl); free(e); fclose(f); return; }
        if (e[i].size > maxsize) maxsize = e[i].size;
    }
    PakEnt *s = malloc(sizeof(PakEnt) * n);
    memcpy(s, e, sizeof(PakEnt) * n);
    qsort(s, n, sizeof(PakEnt), cmp_start);
    for (uint32_t k = 1; k < n; k++)
        if (s[k].start < s[k - 1].start + s[k - 1].size) { err("overlapping entries in .pak - not supported"); free(tbl); free(e); free(s); fclose(f); return; }

    char *dir = dirname_of(path);
    char *stem = strip_ext(basename_of(path));
    char *sub = malloc(strlen(stem) + 20); sprintf(sub, "%s_pak_unpacked", stem);
    char *outdir = join(dir, sub);
    char *filesdir = join(outdir, "files");
    char *ip = join(outdir, "pakinfo.bin");
    char *xp = join(filesdir, "_index.txt");
    FILE *info = NULL, *idx = NULL;
    unsigned char *buf = malloc(maxsize > (1u << 20) ? maxsize : (1u << 20));
    char **names = calloc(n, sizeof(char *));
    if (u8exists(outdir)) {
        err("folder already exists: %s\n         Delete or rename it first (so your edits are never overwritten).", outdir);
        goto done;
    }
    u8mkdir(outdir); u8mkdir(filesdir);
    info = u8fopen(ip, "wb");
    idx = u8fopen(xp, "wb");
    if (!info || !idx) { err("cannot create output files"); goto done; }
    /* pakinfo: "PAKTOOL1" | header | table | n x name | (n+1) pads (u64 len + bytes) */
    fwrite("PAKTOOL1", 1, 8, info);
    fwrite(hdr, 1, PAK_HDR, info);
    fwrite(tbl, 1, tl, info);

    /* names (need first bytes for extension): read in sorted order */
    fprintf(idx, "# FFTool pak index - informational. table#<TAB>hash<TAB>size<TAB>file\r\n");
    /* pad0: table end -> first file */
    {
        uint64_t p0 = s[0].start - (PAK_HDR + tl);
        FSEEK64(f, PAK_HDR + tl, SEEK_SET);
        /* names are written after we know them, so stash pads in a temp file */
    }
    char *tmpp = join(outdir, "pakinfo.tmp");
    FILE *pads = u8fopen(tmpp, "wb");
    if (!pads) { err("cannot create temp file"); free(tmpp); goto done; }
    {
        uint64_t p0 = s[0].start - (PAK_HDR + tl);
        fwrite_u64(pads, p0);
        if (!copy_bytes(f, pads, p0, buf, maxsize > (1u << 20) ? maxsize : (1u << 20))) { err("read error"); fclose(pads); free(tmpp); goto done; }
    }
    for (uint32_t k = 0; k < n; k++) {
        FSEEK64(f, s[k].start, SEEK_SET);
        if (fread(buf, 1, s[k].size, f) != s[k].size) { err("read error at entry %d", s[k].idx); fclose(pads); free(tmpp); goto done; }
        char nm[64];
        int dup = 0;
        for (uint32_t j = k; j-- > 0 && j + 64 > k;) if (s[j].hash == s[k].hash) dup++;
        if (dup) sprintf(nm, "%08X_%d.%s", s[k].hash, dup, guess_ext(buf, s[k].size));
        else sprintf(nm, "%08X.%s", s[k].hash, guess_ext(buf, s[k].size));
        names[s[k].idx] = xstrdup(nm);
        char *fp = join(filesdir, nm);
        if (!write_file(fp, buf, s[k].size)) err("cannot write %s", fp);
        free(fp);
        uint64_t end = s[k].start + s[k].size;
        uint64_t next = k + 1 < n ? s[k + 1].start : fsize;
        fwrite_u64(pads, next - end);
        if (!copy_bytes(f, pads, next - end, buf, maxsize > (1u << 20) ? maxsize : (1u << 20))) { err("read error"); fclose(pads); free(tmpp); goto done; }
        if (((k + 1) & 511) == 0 || k + 1 == n) { printf("\r  Files  : %u / %u", k + 1, n); fflush(stdout); }
    }
    printf("\n");
    if (fclose(pads)) { err("write error (disk full?)"); free(tmpp); goto done; }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t l = (uint16_t)strlen(names[i]);
        unsigned char lb[2] = { (unsigned char)(l >> 8), (unsigned char)l };
        fwrite(lb, 1, 2, info); fwrite(names[i], 1, l, info);
        fprintf(idx, "%u\t%08X\t%u\t%s\r\n", i, e[i].hash, e[i].size, names[i]);
    }
    /* append pads */
    pads = u8fopen(tmpp, "rb");
    if (pads) {
        size_t r;
        while ((r = fread(buf, 1, 1u << 20, pads)) > 0) fwrite(buf, 1, r, info);
        fclose(pads);
    }
#ifdef _WIN32
    { wchar_t *w = widen(tmpp); _wremove(w); free(w); }
#else
    remove(tmpp);
#endif
    free(tmpp);
    if (fclose(info)) { info = NULL; err("write error (disk full?)"); goto done; }
    info = NULL;
    printf("  Output folder : %s\n", outdir);
    printf("  Sounds are in files\\ named by their hash. Replace any of them and drop the folder back on the exe.\n");
done:
    if (info) fclose(info);
    if (idx) fclose(idx);
    fclose(f);
    for (uint32_t i = 0; i < n; i++) free(names[i]);
    free(names); free(buf); free(tbl); free(e); free(s);
    free(dir); free(stem); free(sub); free(outdir); free(filesdir); free(ip); free(xp);
}

static void do_pak_repack(const char *folder) {
    char *dirp = xstrdup(folder);
    strip_trailing_sep(dirp);
    printf("\n== Rebuilding .pak from %s\n", basename_of(dirp));
    char *ip = join(dirp, "pakinfo.bin");
    char *filesdir = join(dirp, "files");
    char *parent = dirname_of(dirp);
    char *fname = xstrdup(basename_of(dirp));
    size_t fnl = strlen(fname);
    if (fnl > 13 && ends_with_ci(fname, "_pak_unpacked")) fname[fnl - 13] = 0;
    char *outname = malloc(strlen(fname) + 20); sprintf(outname, "%s_rebuilt.pak", fname);
    char *origname = malloc(strlen(fname) + 8); sprintf(origname, "%s.pak", fname);
    char *outp = join(parent, outname), *origp = join(parent, origname);
    FILE *info = u8fopen(ip, "rb"), *out = NULL;
    unsigned char hdr[PAK_HDR], *tbl = NULL, *buf = NULL;
    PakEnt *e = NULL, *s = NULL;
    char **names = NULL;
    uint64_t *newsize = NULL;
    uint32_t n = 0;
    char magic[8];
    if (!info || fread(magic, 1, 8, info) != 8 || memcmp(magic, "PAKTOOL1", 8) || fread(hdr, 1, PAK_HDR, info) != PAK_HDR) {
        err("pakinfo.bin missing or invalid (unpack the .pak with this tool first)"); goto done;
    }
    n = be32(hdr + 8);
    uint32_t align = be32(hdr + 12), dstart = be32(hdr + 16);
    size_t tl = 12u * n;
    tbl = malloc(tl);
    if (fread(tbl, 1, tl, info) != tl) { err("pakinfo.bin truncated"); goto done; }
    e = malloc(sizeof(PakEnt) * n); s = malloc(sizeof(PakEnt) * n);
    names = calloc(n, sizeof(char *)); newsize = calloc(n, sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) {
        e[i].hash = be32(tbl + 12 * i); e[i].sector = be32(tbl + 12 * i + 4); e[i].size = be32(tbl + 12 * i + 8);
        e[i].start = dstart + (uint64_t)e[i].sector * align; e[i].idx = (int)i;
        unsigned char lb[2];
        if (fread(lb, 1, 2, info) != 2) { err("pakinfo.bin truncated"); goto done; }
        uint16_t l = (uint16_t)((lb[0] << 8) | lb[1]);
        names[i] = malloc(l + 1);
        if (fread(names[i], 1, l, info) != l) { err("pakinfo.bin truncated"); goto done; }
        names[i][l] = 0;
    }
    memcpy(s, e, sizeof(PakEnt) * n);
    qsort(s, n, sizeof(PakEnt), cmp_start);

    /* pass 1: sizes + layout */
    int changed = 0;
    uint32_t maxsz = 1u << 20;
    for (uint32_t i = 0; i < n; i++) {
        char *fp = join(filesdir, names[i]);
        int ok;
        newsize[i] = file_size64(fp, &ok);
        free(fp);
        if (!ok) { err("missing file files\\%s", names[i]); goto done; }
        if (newsize[i] > 0xFFFFFFFFu) { err("%s is too large", names[i]); goto done; }
        if (newsize[i] != e[i].size) { changed++; printf("  Resized %s (%u -> %llu bytes)\n", names[i], e[i].size, (unsigned long long)newsize[i]); }
        if (newsize[i] > maxsz) maxsz = (uint32_t)newsize[i];
    }
    buf = malloc(maxsz);

    /* the pads follow the names in pakinfo; first u64 is pad0 */
    long long padbase = FTELL64(info);
    unsigned char *newtbl = malloc(tl);
    memcpy(newtbl, tbl, tl);
    {
        unsigned char b8[8];
        if (fread(b8, 1, 8, info) != 8) { err("pakinfo.bin truncated"); free(newtbl); goto done; }
        uint64_t p0 = ((uint64_t)be32(b8) << 32) | be32(b8 + 4);
        uint64_t pos = PAK_HDR + tl + p0;
        FSEEK64(info, (long long)p0, SEEK_CUR);
        for (uint32_t k = 0; k < n; k++) {
            int i = s[k].idx;
            if ((pos - dstart) % align) { err("internal alignment error"); free(newtbl); goto done; }
            uint64_t sec = (pos - dstart) / align;
            if (sec > 0xFFFFFFFFu) { err("archive too large"); free(newtbl); goto done; }
            wbe32(newtbl + 12 * i + 4, (uint32_t)sec);
            wbe32(newtbl + 12 * i + 8, (uint32_t)newsize[i]);
            if (fread(b8, 1, 8, info) != 8) { err("pakinfo.bin truncated"); free(newtbl); goto done; }
            uint64_t pl = ((uint64_t)be32(b8) << 32) | be32(b8 + 4);
            FSEEK64(info, (long long)pl, SEEK_CUR);
            uint64_t end = pos + newsize[i];
            if (newsize[i] == e[i].size) pos = end + pl;
            else pos = end + ((align - (end - dstart) % align) % align);
        }
    }

    /* pass 2: write */
    out = u8fopen(outp, "wb");
    if (!out) { err("cannot create %s", outp); free(newtbl); goto done; }
    fwrite(hdr, 1, PAK_HDR, out);
    fwrite(newtbl, 1, tl, out);
    free(newtbl);
    FSEEK64(info, padbase, SEEK_SET);
    {
        unsigned char b8[8];
        fread(b8, 1, 8, info);
        uint64_t p0 = ((uint64_t)be32(b8) << 32) | be32(b8 + 4);
        if (!copy_bytes(info, out, p0, buf, maxsz)) { err("pakinfo.bin truncated"); goto done; }
        uint64_t pos = PAK_HDR + tl + p0;
        static const unsigned char zeros[4096] = {0};
        for (uint32_t k = 0; k < n; k++) {
            int i = s[k].idx;
            char *fp = join(filesdir, names[i]);
            FILE *in = u8fopen(fp, "rb");
            free(fp);
            if (!in || fread(buf, 1, (size_t)newsize[i], in) != newsize[i]) { if (in) fclose(in); err("cannot read files\\%s", names[i]); goto done; }
            fclose(in);
            if (fwrite(buf, 1, (size_t)newsize[i], out) != newsize[i]) { err("write error (disk full?)"); goto done; }
            uint64_t end = pos + newsize[i];
            fread(b8, 1, 8, info);
            uint64_t pl = ((uint64_t)be32(b8) << 32) | be32(b8 + 4);
            if (newsize[i] == e[i].size) {
                if (!copy_bytes(info, out, pl, buf, maxsz)) { err("pakinfo.bin truncated"); goto done; }
                pos = end + pl;
            } else {
                FSEEK64(info, (long long)pl, SEEK_CUR);
                uint64_t z = (align - (end - dstart) % align) % align;
                for (uint64_t w = z; w;) { size_t c = w < sizeof zeros ? (size_t)w : sizeof zeros; fwrite(zeros, 1, c, out); w -= c; }
                pos = end + z;
            }
            if (((k + 1) & 511) == 0 || k + 1 == n) { printf("\r  Files  : %u / %u", k + 1, n); fflush(stdout); }
        }
        printf("\n");
    }
    if (fclose(out)) { out = NULL; err("write error (disk full?)"); goto done; }
    out = NULL;
    printf("  Wrote  : %s\n", outp);
    if (!changed) printf("  (no files changed size)\n");
    if (u8exists(origp)) {
        printf("  Comparing with %s ...\n", origname);
        uint64_t at = 0;
        int r = compare_files(origp, outp, &at);
        if (r == 1) printf("  Check  : IDENTICAL to %s (byte-for-byte)\n", origname);
        else if (r == 0) printf("  Check  : differs from %s (first difference at 0x%llX) - expected if you replaced files.\n", origname, (unsigned long long)at);
    }
done:
    if (info) fclose(info);
    if (out) fclose(out);
    if (names) for (uint32_t i = 0; i < n; i++) free(names[i]);
    free(names); free(newsize); free(tbl); free(e); free(s); free(buf);
    free(dirp); free(ip); free(filesdir); free(parent); free(fname); free(outname); free(origname); free(outp); free(origp);
}

/* ------------------------------------------------------------------ */
static void handle(const char *path) {
    if (u8isdir(path)) {
        char *pi = join(path, "pakinfo.bin");
        int pak = u8exists(pi);
        free(pi);
        if (pak) do_pak_repack(path); else do_repack(path);
        return;
    }
    if (ends_with_ci(path, "pakinfo.bin")) { char *d = dirname_of(path); do_pak_repack(d); free(d); return; }
    if (ends_with_ci(path, ".zone") || ends_with_ci(path, "ffinfo.bin")) {
        char *d = dirname_of(path); do_repack(d); free(d); return;
    }
    unsigned char m[4] = {0};
    FILE *f = u8fopen(path, "rb");
    if (f) { fread(m, 1, 4, f); fclose(f); }
    if (!memcmp(m, "0KAP", 4)) do_pak_unpack(path);
    else do_unpack(path);
}

static int real_main(int argc, char **argv) {
    printf("FFTool %s - CoD WaW PS3 fastfile + .pak unpacker/repacker (byte-exact)\n", TOOL_VER);
    if (argc < 2) {
        printf("\nDrag and drop onto this .exe:\n"
               "  * a .ff file             -> unpacks to NAME_unpacked\\ (zone + extracted scripts)\n"
               "  * a NAME_unpacked folder -> rebuilds NAME_rebuilt.ff (identical if unedited)\n"
               "  * a .zone file           -> same as dropping its folder\n"
               "    (models -> models\\*.obj for Blender, textures -> textures\\*.dds)\n"
               "  * a .pak sound archive   -> unpacks to NAME_pak_unpacked\\files\\ (mp3s etc.)\n"
               "  * a NAME_pak_unpacked folder -> rebuilds NAME_rebuilt.pak (identical if unedited)\n"
               "\nEdited scripts in rawfiles\\ are put back on rebuild (same size or smaller).\n"
               "Replaced sounds in a .pak can be any size; the table is updated automatically.\n");
    }
    for (int i = 1; i < argc; i++) handle(argv[i]);
    printf("\n%s\n", g_errors ? "Finished with errors." : "Done.");
    return g_errors ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv) {
    SetConsoleOutputCP(CP_UTF8);
    char **argv = malloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) argv[i] = narrow(wargv[i]);
    argv[argc] = NULL;
    int r = real_main(argc, argv);
    DWORD pl[4];
    if (GetConsoleProcessList(pl, 4) <= 1) { /* own console window (drag & drop / double click) */
        printf("\nPress Enter to close...");
        fflush(stdout);
        getchar();
    }
    return r;
}
#else
int main(int argc, char **argv) { return real_main(argc, argv); }
#endif
