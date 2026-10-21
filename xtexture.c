/*
 * xtexture.c - GfxImage (CoD WaW PS3) export to .dds and re-import.
 * Included from fftool.c after xmodel.c.
 *
 *   Image header (stream, unpadded):
 *     +0  u32 mapType   +4 CellGcmTexture (24 bytes: format, mipmap, dimension, cubemap,
 *     remap, width, height, depth, location, pad, pitch, offset)   +28 u32 semantic/flags
 *     +32 u32 cardMemory  +36 w,h,d   +42 2 bytes   +44 pixels* (-1)   +48 name* , name inline
 *   Pixel data for ALL images is stored at the end of the zone stream (block 3),
 *   in header order, each image padded to 128 bytes (cubemaps: each face padded).
 *   DXT data is little-endian (same as DDS). Uncompressed formats are swizzled (Morton).
 */

typedef struct {
    size_t hdr, data;
    int fmt, mips, cube, w, h;
    uint32_t cm;
    char name[160];
    char file[200];
} XImage;

static int tex_is_dxt(int f) { return f == 0x86 || f == 0x87 || f == 0x88; }
static int tex_supported(int f) { return tex_is_dxt(f) || f == 0x85 || f == 0x9e || f == 0x81; }
static int tex_bpp(int f) { return f == 0x81 ? 1 : 4; }

static size_t tex_level_size(int fmt, int w, int h, int l) {
    int lw = w >> l, lh = h >> l;
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    if (tex_is_dxt(fmt)) return (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * (fmt == 0x86 ? 8 : 16);
    return (size_t)lw * lh * tex_bpp(fmt);
}
static size_t tex_chain_size(const XImage *im) {
    size_t s = 0;
    for (int l = 0; l < im->mips; l++) s += tex_level_size(im->fmt, im->w, im->h, l);
    return s;
}
static size_t tex_face_stride(const XImage *im) { return im->cube ? al_up(tex_chain_size(im), 128) : tex_chain_size(im); }

static int is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

/* Morton index for RSX swizzled textures */
static size_t swz_index(int x, int y, int w, int h) {
    int lw = 0, lh = 0;
    while ((1 << lw) < w) lw++;
    while ((1 << lh) < h) lh++;
    int m = lw < lh ? lw : lh;
    size_t idx = 0;
    for (int b = 0; b < m; b++) {
        idx |= (size_t)((x >> b) & 1) << (2 * b);
        idx |= (size_t)((y >> b) & 1) << (2 * b + 1);
    }
    if (lw > lh) idx |= (size_t)(x >> m) << (2 * m);
    else idx |= (size_t)(y >> m) << (2 * m);
    return idx;
}

/* convert one uncompressed level between PS3 (swizzled, big-endian ARGB) and DDS (linear, BGRA) */
static void level_ps3_to_dds(const unsigned char *src, unsigned char *dst, int fmt, int w, int h) {
    int bpp = tex_bpp(fmt), swz = is_pow2(w) && is_pow2(h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t si = swz ? swz_index(x, y, w, h) : (size_t)y * w + x;
            const unsigned char *s = src + si * bpp;
            unsigned char *d = dst + ((size_t)y * w + x) * bpp;
            if (bpp == 1) d[0] = s[0];
            else { d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0]; }
        }
}
static void level_dds_to_ps3(const unsigned char *src, unsigned char *dst, int fmt, int w, int h) {
    int bpp = tex_bpp(fmt), swz = is_pow2(w) && is_pow2(h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t di = swz ? swz_index(x, y, w, h) : (size_t)y * w + x;
            const unsigned char *s = src + ((size_t)y * w + x) * bpp;
            unsigned char *d = dst + di * bpp;
            if (bpp == 1) d[0] = s[0];
            else { d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0]; }
        }
}

static XImage *scan_images(const unsigned char *z, size_t zl, int *count) {
    int cap = 256, n = 0;
    XImage *im = malloc(sizeof(XImage) * cap);
    uint64_t total = 0;
    for (size_t g = 0; g + 60 < zl; g++) {
        if (z[g] || z[g + 1] || z[g + 2] || z[g + 3] > 7) continue;
        int fmt = z[g + 4], mip = z[g + 5], dim = z[g + 6], cube = z[g + 7];
        if (fmt < 0x81 || fmt > 0xbf || mip < 1 || mip > 14 || dim < 1 || dim > 3 || cube > 1) continue;
        int w = (z[g + 12] << 8) | z[g + 13], h = (z[g + 14] << 8) | z[g + 15], d = (z[g + 16] << 8) | z[g + 17];
        if (!w || !h || !d || w > 8192 || h > 8192) continue;
        if (memcmp(z + g + 36, z + g + 12, 6)) continue;
        if (be32(z + g + 44) != 0xFFFFFFFFu) continue;
        if (n == cap) { cap *= 2; im = realloc(im, sizeof(XImage) * cap); }
        XImage *x = &im[n];
        memset(x, 0, sizeof *x);
        x->hdr = g; x->fmt = fmt; x->mips = mip; x->cube = cube; x->w = w; x->h = h;
        x->cm = be32(z + g + 32);
        if (be32(z + g + 48) == 0xFFFFFFFFu) {
            size_t i = 0;
            while (g + 52 + i < zl && z[g + 52 + i] && i < sizeof x->name - 1) { x->name[i] = (char)z[g + 52 + i]; i++; }
            x->name[i] = 0;
        }
        total += x->cm;
        n++;
        g += 51;
    }
    /* pixel data sits at the end of the stream: [36-byte header][stream of be32(z) bytes] */
    uint64_t stream_end = 36ull + be32(z);
    if (n == 0 || total != be32(z + 0x14) || total > stream_end || stream_end > zl) {
        free(im); *count = 0;
        return NULL;
    }
    size_t off = (size_t)(stream_end - total);
    for (int i = 0; i < n; i++) { im[i].data = off; off += im[i].cm; }
    *count = n;
    return im;
}

static void tex_assign_files(XImage *im, int n) {
    for (int i = 0; i < n; i++) {
        char base[160];
        if (im[i].name[0]) safe_name(im[i].name, base);
        else snprintf(base, sizeof base, "unnamed_%03d", i);
        snprintf(im[i].file, sizeof im[i].file, "%s.dds", base);
        int dup = 0;
        for (int j = 0; j < i; j++) if (!strcmp(im[j].file, im[i].file)) dup++;
        if (dup) snprintf(im[i].file, sizeof im[i].file, "%s_%d.dds", base, dup + 1);
    }
}

/* ---- DDS ---- */
static void le32w(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t le32r(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void dds_header(unsigned char *h, const XImage *im) {
    memset(h, 0, 128);
    memcpy(h, "DDS ", 4);
    le32w(h + 4, 124);
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | (im->mips > 1 ? 0x20000 : 0) | (tex_is_dxt(im->fmt) ? 0x80000 : 0x8);
    le32w(h + 8, flags);
    le32w(h + 12, im->h); le32w(h + 16, im->w);
    le32w(h + 20, tex_is_dxt(im->fmt) ? (uint32_t)tex_level_size(im->fmt, im->w, im->h, 0) : (uint32_t)(im->w * tex_bpp(im->fmt)));
    le32w(h + 28, im->mips);
    le32w(h + 76, 32);                         /* pixel format */
    if (tex_is_dxt(im->fmt)) {
        le32w(h + 80, 0x4);
        memcpy(h + 84, im->fmt == 0x86 ? "DXT1" : im->fmt == 0x87 ? "DXT3" : "DXT5", 4);
    } else if (im->fmt == 0x81) {
        le32w(h + 80, 0x20000); le32w(h + 88, 8); le32w(h + 92, 0xFF);
    } else {
        le32w(h + 80, im->fmt == 0x85 ? 0x41 : 0x40); le32w(h + 88, 32);
        le32w(h + 92, 0x00FF0000); le32w(h + 96, 0x0000FF00); le32w(h + 100, 0x000000FF);
        le32w(h + 104, im->fmt == 0x85 ? 0xFF000000 : 0);
    }
    le32w(h + 108, 0x1000 | (im->mips > 1 ? 0x400008 : 0) | (im->cube ? 0x8 : 0));
    if (im->cube) le32w(h + 112, 0x200 | 0xFC00);
}

/* build the DDS payload (faces x mips) from zone data */
static unsigned char *tex_to_dds(const unsigned char *z, const XImage *im, size_t *outlen) {
    int faces = im->cube ? 6 : 1;
    size_t chain = tex_chain_size(im), stride = tex_face_stride(im);
    unsigned char *buf = malloc(128 + chain * faces);
    dds_header(buf, im);
    size_t o = 128;
    for (int f = 0; f < faces; f++) {
        const unsigned char *src = z + im->data + stride * f;
        if (tex_is_dxt(im->fmt)) { memcpy(buf + o, src, chain); o += chain; continue; }
        size_t lo = 0;
        for (int l = 0; l < im->mips; l++) {
            int lw = im->w >> l, lh = im->h >> l;
            if (lw < 1) lw = 1;
            if (lh < 1) lh = 1;
            level_ps3_to_dds(src + lo, buf + o, im->fmt, lw, lh);
            size_t ls = tex_level_size(im->fmt, im->w, im->h, l);
            lo += ls; o += ls;
        }
    }
    *outlen = o;
    return buf;
}

static int textures_export(const unsigned char *z, size_t zl, const char *outdir) {
    int n;
    XImage *im = scan_images(z, zl, &n);
    if (!im) { printf("  (no texture data recognised in this zone)\n"); return 0; }
    tex_assign_files(im, n);
    char *tdir = join(outdir, "textures");
    u8mkdir(tdir);
    char *ip = join(tdir, "_index.txt");
    FILE *idx = u8fopen(ip, "wb");
    if (idx) fprintf(idx, "# FFTool texture list - do not edit. index<TAB>file<TAB>format<TAB>size<TAB>mips\r\n");
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (!tex_supported(im[i].fmt)) continue;
        if (im[i].data + tex_face_stride(&im[i]) * (im[i].cube ? 6 : 1) > zl) continue;
        size_t len;
        unsigned char *dds = tex_to_dds(z, &im[i], &len);
        char *fp = join(tdir, im[i].file);
        if (write_file(fp, dds, len)) done++;
        else err("cannot write textures\\%s", im[i].file);
        free(fp); free(dds);
        const char *fn = im[i].fmt == 0x86 ? "DXT1" : im[i].fmt == 0x87 ? "DXT3" : im[i].fmt == 0x88 ? "DXT5" : im[i].fmt == 0x81 ? "L8" : im[i].fmt == 0x85 ? "A8R8G8B8" : "X8R8G8B8";
        if (idx) fprintf(idx, "%d\t%s\t%s%s\t%dx%d\t%d\r\n", i, im[i].file, fn, im[i].cube ? " cube" : "", im[i].w, im[i].h, im[i].mips);
    }
    if (idx) fclose(idx);
    free(ip); free(tdir); free(im);
    return done;
}

/* returns changed count or -1 on error */
static int textures_import(unsigned char *z, size_t zl, const char *dir) {
    char *tdir = join(dir, "textures");
    if (!u8isdir(tdir)) { free(tdir); return 0; }
    int n;
    XImage *im = scan_images(z, zl, &n);
    if (!im) { free(tdir); return 0; }
    tex_assign_files(im, n);
    int changed = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        if (!tex_supported(im[i].fmt)) continue;
        char *fp = join(tdir, im[i].file);
        size_t len;
        unsigned char *d = read_file(fp, &len);
        free(fp);
        if (!d) continue;
        size_t olen;
        unsigned char *orig = tex_to_dds(z, &im[i], &olen);
        if (len == olen && !memcmp(d, orig, len)) { free(d); free(orig); continue; }
        free(orig);
        /* parse DDS */
        if (len < 128 || memcmp(d, "DDS ", 4)) { err("textures\\%s is not a DDS file", im[i].file); bad++; free(d); continue; }
        uint32_t hh = le32r(d + 12), ww = le32r(d + 16), mips = le32r(d + 28), pf = le32r(d + 80), bits = le32r(d + 88);
        if (!(le32r(d + 8) & 0x20000) || mips == 0) mips = 1;
        size_t hdr = 128;
        if ((pf & 4) && !memcmp(d + 84, "DX10", 4)) hdr = 148;
        int fmt_ok;
        if (tex_is_dxt(im[i].fmt)) {
            const char *want = im[i].fmt == 0x86 ? "DXT1" : im[i].fmt == 0x87 ? "DXT3" : "DXT5";
            fmt_ok = (pf & 4) && !memcmp(d + 84, want, 4);
        } else if (im[i].fmt == 0x81) fmt_ok = !(pf & 4) && bits == 8;
        else fmt_ok = !(pf & 4) && bits == 32 && le32r(d + 92) == 0x00FF0000 && le32r(d + 100) == 0x000000FF;
        if (!fmt_ok || (int)ww != im[i].w || (int)hh != im[i].h) {
            const char *want = im[i].fmt == 0x86 ? "DXT1 (BC1)" : im[i].fmt == 0x87 ? "DXT3 (BC2)" : im[i].fmt == 0x88 ? "DXT5 (BC3)" : im[i].fmt == 0x81 ? "L8 / 8-bit luminance" : "A8R8G8B8 (32-bit, uncompressed)";
            err("textures\\%s must be saved as %s, %dx%d (got %ux%u). Not written.", im[i].file, want, im[i].w, im[i].h, ww, hh);
            bad++; free(d); continue;
        }
        int faces = im[i].cube ? 6 : 1;
        int use = (int)mips < im[i].mips ? (int)mips : im[i].mips;
        size_t dds_chain = 0;
        for (int l = 0; l < (int)mips; l++) dds_chain += tex_level_size(im[i].fmt, im[i].w, im[i].h, l);
        if (hdr + dds_chain * faces > len) { err("textures\\%s is truncated", im[i].file); bad++; free(d); continue; }
        size_t stride = tex_face_stride(&im[i]);
        for (int f = 0; f < faces; f++) {
            const unsigned char *src = d + hdr + dds_chain * f;
            unsigned char *dst = z + im[i].data + stride * f;
            size_t so = 0;
            for (int l = 0; l < use; l++) {
                int lw = im[i].w >> l, lh = im[i].h >> l;
                if (lw < 1) lw = 1;
                if (lh < 1) lh = 1;
                size_t ls = tex_level_size(im[i].fmt, im[i].w, im[i].h, l);
                if (tex_is_dxt(im[i].fmt)) memcpy(dst + so, src + so, ls);
                else level_dds_to_ps3(src + so, dst + so, im[i].fmt, lw, lh);
                so += ls;
            }
        }
        if (use < im[i].mips)
            printf("  Texture %s: replaced (only %d of %d mip levels in the DDS - save with mipmaps for best results)\n", im[i].file, use, im[i].mips);
        else printf("  Texture %s: replaced\n", im[i].file);
        changed++;
        free(d);
    }
    free(im); free(tdir);
    return bad ? -1 : changed;
}
