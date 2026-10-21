/*
 * xmodel.c - XModel (CoD WaW PS3 zone) export to OBJ and in-place re-import.
 * Included from fftool.c (uses its helpers: join, read_file, write_file, err, be32 ...).
 *
 * Zone stream facts used here (reverse engineered, PS3 T4):
 *   XModel struct = 212 bytes, name follows inline.  Stream is unpadded.
 *   XSurface = 80 bytes: +2 vertCount, +4 triCount, +8 triIndices*, +12 vertCount[4],
 *            +20 vertsBlend*, +24 verts0*, +36 verts1*, +48 vertListCount, +52 vertList*
 *   verts0 = 16 B/vertex : float x,y,z, binormal sign
 *   verts1 = 16 B/vertex : rgba, half u, half v, packed normal (11:11:10), packed tangent
 *   Per-surface stream order: blend, verts0, verts1, vertList(+collision trees), tris.
 *   Pointers that are not -1 reference earlier data: ((block<<28)|offset)+1.
 *   Block 8 holds everything except verts1 of single-vertList rigid surfaces (block 12).
 */
#include <math.h>

#define XFF 0xFFFFFFFFu
enum { K_BLEND, K_V0, K_V1, K_VLIST, K_TREE, K_NODES, K_LEAFS, K_TRI };

typedef struct { int surf, kind; size_t pos; uint32_t size; int align; int trimax; } XItem;
typedef struct {
    uint16_t vc, tc, vci[4];
    uint32_t f[12];               /* fields at +8,+20,+24,+28,+32,+36,+40..+60 */
    long long own[3];             /* inline pos of v0, v1, tri (-1 = not inline) */
    long long src[3];             /* resolved pos of v0, v1, tri */
    int srcmodel[3];
} XSurf;
typedef struct {
    char name[256];
    size_t start, end;
    int ns, owns;
    XSurf *s;
    XItem *it[2]; int nit[2], cap[2];   /* [0]=block 8, [1]=block 12 */
    int anc_idx[2]; uint32_t anc[2]; int has_anc[2];
} XModel;

static const unsigned char *XZ;
static size_t XZL;
static uint32_t xu32(size_t p) { return be32(XZ + p); }
static uint16_t xu16(size_t p) { return (uint16_t)((XZ[p] << 8) | XZ[p + 1]); }
static size_t al_up(size_t x, int a) { return (x + (size_t)a - 1) & ~((size_t)a - 1); }

static void add_item(XModel *m, int b, int surf, int kind, size_t pos, uint32_t size, int align) {
    if (m->nit[b] == m->cap[b]) { m->cap[b] = m->cap[b] ? m->cap[b] * 2 : 32; m->it[b] = realloc(m->it[b], sizeof(XItem) * m->cap[b]); }
    XItem *x = &m->it[b][m->nit[b]++];
    x->surf = surf; x->kind = kind; x->pos = pos; x->size = size; x->align = align; x->trimax = -1;
}

static int name_ok(size_t p, char *out) {
    size_t i = 0;
    while (p + i < XZL && i < 255) {
        unsigned char c = XZ[p + i];
        if (!c) break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '@' || c == '-' || c == '.' || c == '/' || c == '#' || c == '~' || c == '&' || c == '$'))
            return 0;
        out[i] = (char)c; i++;
    }
    if (i == 0 || i >= 255 || p + i >= XZL) return 0;
    out[i] = 0;
    return (int)i + 1;
}

/* parse one XModel at p; returns 1 on success */
static int parse_xmodel(size_t p, XModel *m) {
    memset(m, 0, sizeof *m);
    if (p + 212 + 2 > XZL) return 0;
    if (xu32(p) != XFF) return 0;
    int nb = XZ[p + 4], nr = XZ[p + 5], ns = XZ[p + 6], lr = XZ[p + 7];
    if (ns == 0 || lr > 2 || nr > nb) return 0;
    uint32_t ptr[8];
    for (int i = 0; i < 8; i++) {
        ptr[i] = xu32(p + 8 + 4 * i);
        if (ptr[i] != XFF && (ptr[i] >> 28) != 8) return 0;
    }
    uint32_t dbits = xu32(p + 40);
    float d; memcpy(&d, &dbits, 4);
    if (!(d >= 1.0f && d <= 1e7f)) return 0;
    if (ptr[6] != XFF) return 0;                 /* surfaces not inline */
    int nl = name_ok(p + 212, m->name);
    if (!nl) return 0;
    size_t q = p + 212 + nl;
    int nc = nb - nr;
    if (ptr[0] == XFF) q += 2 * nb;
    if (ptr[1] == XFF) q += nc;
    if (ptr[2] == XFF) q += 8 * nc;
    if (ptr[3] == XFF) q += 16 * nc;
    if (ptr[4] == XFF) q += nb;
    if (ptr[5] == XFF) q += 32 * nb;
    size_t sa = q; q += 80 * (size_t)ns;
    if (q > XZL) return 0;
    m->start = p; m->ns = ns;
    m->s = calloc(ns, sizeof(XSurf));
    static const int foff[12] = { 8, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60 };
    for (int i = 0; i < ns; i++) {
        XSurf *s = &m->s[i];
        size_t b = sa + 80 * (size_t)i;
        s->vc = xu16(b + 2); s->tc = xu16(b + 4);
        for (int k = 0; k < 4; k++) s->vci[k] = xu16(b + 12 + 2 * k);
        for (int k = 0; k < 12; k++) s->f[k] = xu32(b + foff[k]);
        for (int k = 0; k < 3; k++) { s->own[k] = -1; s->src[k] = -1; s->srcmodel[k] = -1; }
    }
    for (int i = 0; i < ns; i++) {
        XSurf *s = &m->s[i];
        uint32_t tri = s->f[0], blend = s->f[1], v0 = s->f[2], v1 = s->f[5], vlc = s->f[8], vl = s->f[9];
        if (vlc > 64) goto bad;
        if (blend == XFF) {
            uint32_t n = 2u * (s->vci[0] + 3u * s->vci[1] + 5u * s->vci[2] + 7u * s->vci[3]);
            add_item(m, 0, i, K_BLEND, q, n, 2); q += n;
        }
        if (v0 == XFF) { add_item(m, 0, i, K_V0, q, 16u * s->vc, 16); s->own[0] = (long long)q; q += 16u * s->vc; }
        if (v1 == XFF) { add_item(m, vlc == 1 ? 1 : 0, i, K_V1, q, 16u * s->vc, 16); s->own[1] = (long long)q; q += 16u * s->vc; }
        if (q > XZL) goto bad;
        if (vl == XFF && vlc) {
            uint32_t ents[64];
            for (uint32_t k = 0; k < vlc; k++) ents[k] = xu32(q + 8 + 12 * k);
            add_item(m, 0, i, K_VLIST, q, 12 * vlc, 4); q += 12 * vlc;
            for (uint32_t k = 0; k < vlc; k++) {
                if (ents[k] != XFF) continue;
                if (q + 40 > XZL) goto bad;
                uint32_t ncn = xu32(q + 24), nf = xu32(q + 28), lc = xu32(q + 32), lf = xu32(q + 36);
                if (ncn > 1000000 || lc > 1000000) goto bad;
                add_item(m, 0, i, K_TREE, q, 40, 4); q += 40;
                if (nf == XFF) { add_item(m, 0, i, K_NODES, q, 16 * ncn, 16); q += 16 * ncn; }
                if (lf == XFF) { add_item(m, 0, i, K_LEAFS, q, 2 * lc, 2); q += 2 * lc; }
            }
        }
        if (tri == XFF) {
            if (q + 6u * s->tc > XZL) goto bad;
            add_item(m, 0, i, K_TRI, q, 6u * s->tc, 16); s->own[2] = (long long)q;
            int mx = 0;
            for (uint32_t k = 0; k < 3u * s->tc; k++) { int v = xu16(q + 2 * k); if (v > mx) mx = v; }
            if (mx >= s->vc) goto bad;
            m->it[0][m->nit[0] - 1].trimax = mx;
            q += 6u * s->tc;
        }
        if (q > XZL) goto bad;
        if (s->own[0] >= 0) m->owns = 1;
    }
    /* validation: material handle array follows */
    if (ptr[7] == XFF) {
        if (q + 4u * ns > XZL) goto bad;
        for (int i = 0; i < ns; i++) { uint32_t h = xu32(q + 4 * i); if (h != XFF && (h >> 28) != 8) goto bad; }
    }
    m->end = q;
    return 1;
bad:
    free(m->s); free(m->it[0]); free(m->it[1]);
    memset(m, 0, sizeof *m);
    return 0;
}

static void free_models(XModel *ms, int n) {
    for (int i = 0; i < n; i++) { free(ms[i].s); free(ms[i].it[0]); free(ms[i].it[1]); }
    free(ms);
}

/* -------------------- pointer resolution -------------------- */
static uint32_t sim_forward(XModel *x, int b, int from, uint32_t A, int to) {
    uint32_t v = A;
    for (int t = from; t < to; t++) { v += x->it[b][t].size; v = (uint32_t)al_up(v, x->it[b][t + 1].align); }
    return v;
}

typedef struct { int m, surf, field; uint32_t ptr; int done; } XRef;

static void resolve_refs(XModel *ms, int nm) {
    int cap = 256, nr = 0;
    XRef *r = malloc(sizeof(XRef) * cap);
    static const int fidx[4] = { 0, 1, 2, 5 };       /* tri, blend, v0, v1 */
    for (int a = 0; a < nm; a++)
        for (int i = 0; i < ms[a].ns; i++)
            for (int k = 0; k < 4; k++) {
                uint32_t p = ms[a].s[i].f[fidx[k]];
                if (p == 0 || p == XFF) continue;
                if ((p >> 28) != 8 && (p >> 28) != 12) continue;
                if (nr == cap) { cap *= 2; r = realloc(r, sizeof(XRef) * cap); }
                r[nr].m = a; r[nr].surf = i; r[nr].field = fidx[k]; r[nr].ptr = p; r[nr].done = 0; nr++;
            }
    int changed = 1, rnd = 0;
    while ((changed || rnd < 2) && rnd < 30) {
        changed = 0; rnd++;
        for (int ri = 0; ri < nr; ri++) {
            if (r[ri].done) continue;
            XModel *m = &ms[r[ri].m];
            XSurf *s = &m->s[r[ri].surf];
            int b = (r[ri].ptr >> 28) == 12 ? 1 : 0;
            uint32_t P = (r[ri].ptr - 1) & 0x0FFFFFFF;
            int kind = r[ri].field == 0 ? K_TRI : r[ri].field == 1 ? K_BLEND : r[ri].field == 2 ? K_V0 : K_V1;
            int ngood = 0, nunk = 0, gx = -1, gj = -1, ux = -1, uj = -1;
            for (int xi = 0; xi < nm && ms[xi].start <= m->start; xi++) {
                XModel *x = &ms[xi];
                for (int j = 0; j < x->nit[b]; j++) {
                    XItem *it = &x->it[b][j];
                    if (it->kind != kind) continue;
                    XSurf *xs = &x->s[it->surf];
                    if (kind == K_TRI && (xs->tc != s->tc || it->trimax >= s->vc)) continue;
                    if ((kind == K_V0 || kind == K_V1) && xs->vc != s->vc) continue;
                    if (kind == K_BLEND && memcmp(xs->vci, s->vci, sizeof s->vci)) continue;
                    if (x == m && it->surf >= r[ri].surf) continue;
                    if (x->has_anc[b]) {
                        int ai = x->anc_idx[b];
                        int ok = j >= ai ? sim_forward(x, b, ai, x->anc[b], j) == P
                                         : sim_forward(x, b, j, P, ai) == x->anc[b];
                        if (ok) { ngood++; gx = xi; gj = j; }
                    } else { nunk++; ux = xi; uj = j; }
                }
            }
            int px = -1, pj = -1;
            if (ngood == 1) { px = gx; pj = gj; }
            else if (ngood == 0 && nunk == 1 && rnd > 1) { px = ux; pj = uj; }
            if (px < 0) continue;
            XModel *x = &ms[px];
            if (!x->has_anc[b]) { x->has_anc[b] = 1; x->anc_idx[b] = pj; x->anc[b] = P; }
            r[ri].done = 1; changed = 1;
            int slot = kind == K_V0 ? 0 : kind == K_V1 ? 1 : kind == K_TRI ? 2 : -1;
            if (slot >= 0) { s->src[slot] = (long long)x->it[b][pj].pos; s->srcmodel[slot] = px; }
        }
    }
    free(r);
}

static XModel *scan_models(const unsigned char *z, size_t zl, int *count) {
    XZ = z; XZL = zl;
    int cap = 64, n = 0;
    XModel *ms = malloc(sizeof(XModel) * cap);
    for (size_t p = 0; p + 220 < zl; p++) {
        if (z[p] != 0xFF || z[p + 1] != 0xFF || z[p + 2] != 0xFF || z[p + 3] != 0xFF) continue;
        if (z[p + 7] > 2) continue;
        XModel m;
        if (!parse_xmodel(p, &m)) continue;
        if (n == cap) { cap *= 2; ms = realloc(ms, sizeof(XModel) * cap); }
        ms[n++] = m;
        p = m.end - 1;
    }
    resolve_refs(ms, n);
    for (int a = 0; a < n; a++)
        for (int i = 0; i < ms[a].ns; i++) {
            XSurf *s = &ms[a].s[i];
            for (int k = 0; k < 3; k++) if (s->own[k] >= 0) { s->src[k] = s->own[k]; s->srcmodel[k] = a; }
        }
    *count = n;
    return ms;
}

/* -------------------- vertex helpers -------------------- */
static float xf32(size_t p) { uint32_t u = xu32(p); float f; memcpy(&f, &u, 4); return f; }
static void put_f32(unsigned char *z, size_t p, float f) { uint32_t u; memcpy(&u, &f, 4); wbe32(z + p, u); }

static float half2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static uint16_t f2half(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000;
        int shift = 14 - e;
        uint32_t hm = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1FFF;
    uint32_t h = sign | ((uint32_t)e << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) h++;
    return (uint16_t)h;
}
static void dec_normal(uint32_t u, float *n) {
    int x = (int)(u & 0x7FF), y = (int)((u >> 11) & 0x7FF), z = (int)((u >> 22) & 0x3FF);
    if (x >= 1024) x -= 2048;
    if (y >= 1024) y -= 2048;
    if (z >= 512) z -= 1024;
    n[0] = x / 1023.0f; n[1] = y / 1023.0f; n[2] = z / 511.0f;
}
static uint32_t enc_normal(const float *n) {
    int x = (int)lrintf(fmaxf(-1.f, fminf(1.f, n[0])) * 1023.f);
    int y = (int)lrintf(fmaxf(-1.f, fminf(1.f, n[1])) * 1023.f);
    int z = (int)lrintf(fmaxf(-1.f, fminf(1.f, n[2])) * 511.f);
    return ((uint32_t)x & 0x7FF) | (((uint32_t)y & 0x7FF) << 11) | (((uint32_t)z & 0x3FF) << 22);
}

static void safe_name(const char *in, char *out) {
    size_t i = 0;
    for (; in[i] && i < 200; i++) {
        char c = in[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '@') ? c : '_';
    }
    out[i] = 0;
}

/* -------------------- export -------------------- */
static int models_export(const unsigned char *z, size_t zl, const char *outdir) {
    int nm;
    XModel *ms = scan_models(z, zl, &nm);
    char *mdir = join(outdir, "models");
    u8mkdir(mdir);
    char *ip = join(mdir, "_index.txt");
    FILE *idx = u8fopen(ip, "wb");
    int exported = 0;
    if (idx) fprintf(idx, "# FFTool model list. Edit the .obj files (keep vertex count/order) and rebuild.\r\n");
    for (int a = 0; a < nm; a++) {
        XModel *m = &ms[a];
        if (!m->owns) {
            int src = -1;
            for (int i = 0; i < m->ns && src < 0; i++) src = m->s[i].srcmodel[0];
            if (idx) fprintf(idx, "%s\tskin variant - uses the mesh of %s\r\n", m->name, src >= 0 ? ms[src].name : "(unknown)");
            continue;
        }
        char fn[256], file[300];
        safe_name(m->name, fn);
        snprintf(file, sizeof file, "%s.obj", fn);
        char *fp = join(mdir, file);
        FILE *f = u8fopen(fp, "wb");
        free(fp);
        if (!f) { err("cannot write models\\%s", file); continue; }
        fprintf(f, "# FFTool export of XModel '%s'\n", m->name);
        fprintf(f, "# Import into Blender with default OBJ settings, edit vertex positions / UVs,\n");
        fprintf(f, "# and export back to this file (OBJ, default settings). Do NOT add or delete vertices.\n");
        fprintf(f, "# Objects named surfNN_shared reuse another model's mesh and are ignored on import.\n");
        int base = 1, tbase = 1, tv = 0, tt = 0, skipped = 0;
        for (int i = 0; i < m->ns; i++) {
            XSurf *s = &m->s[i];
            if (s->src[0] < 0 || s->src[2] < 0) { skipped++; continue; }
            int own = s->own[0] >= 0;
            fprintf(f, "o surf%02d%s\n", i, own ? "" : "_shared");
            size_t v0 = (size_t)s->src[0];
            for (int k = 0; k < s->vc; k++) {
                float x = xf32(v0 + 16 * k), y = xf32(v0 + 16 * k + 4), zz = xf32(v0 + 16 * k + 8);
                fprintf(f, "v %.9g %.9g %.9g\n", x, zz, -y);
            }
            int hasv1 = s->src[1] >= 0;
            if (hasv1) {
                size_t v1 = (size_t)s->src[1];
                for (int k = 0; k < s->vc; k++)
                    fprintf(f, "vt %.9g %.9g\n", half2f(xu16(v1 + 16 * k + 4)), 1.0f - half2f(xu16(v1 + 16 * k + 6)));
                for (int k = 0; k < s->vc; k++) {
                    float n[3]; dec_normal(xu32(v1 + 16 * k + 8), n);
                    fprintf(f, "vn %.6f %.6f %.6f\n", n[0], n[2], -n[1]);
                }
            }
            size_t t = (size_t)s->src[2];
            for (int k = 0; k < s->tc; k++) {
                int a0 = xu16(t + 6 * k) + base, a1 = xu16(t + 6 * k + 2) + base, a2 = xu16(t + 6 * k + 4) + base;
                int b0 = a0 - base + tbase, b1 = a1 - base + tbase, b2 = a2 - base + tbase;
                if (hasv1) fprintf(f, "f %d/%d/%d %d/%d/%d %d/%d/%d\n", a0, b0, b0, a2, b2, b2, a1, b1, b1);
                else fprintf(f, "f %d %d %d\n", a0, a2, a1);
            }
            base += s->vc; if (hasv1) tbase += s->vc; tv += s->vc; tt += s->tc;
        }
        fclose(f);
        exported++;
        if (idx) fprintf(idx, "%s\t%s\t%d surfaces, %d verts, %d tris%s\r\n", m->name, file, m->ns, tv, tt,
                         skipped ? " (some shared surfaces not exported)" : "");
    }
    if (idx) fclose(idx);
    free(ip); free(mdir);
    free_models(ms, nm);
    return exported;
}

/* -------------------- import -------------------- */
typedef struct { float *v; int nv, cv; float *vt; int nvt, cvt; float *vn; int nvn, cvn; } ObjData;
typedef struct { char name[128]; int vstart, vend; int *fv, *ft, *fn; int nf, cf; } ObjObj;

static void push3(float **a, int *n, int *c, int k, const float *x) {
    if (*n == *c) { *c = *c ? *c * 2 : 1024; *a = realloc(*a, sizeof(float) * k * *c); }
    memcpy(*a + (size_t)*n * k, x, sizeof(float) * k); (*n)++;
}

/* returns number of changed surfaces, -1 on error */
static int import_obj(unsigned char *z, XModel *m, const char *path, const char *disp) {
    size_t len;
    char *txt = (char *)read_file(path, &len);
    if (!txt) return 0;
    txt = realloc(txt, len + 1); txt[len] = 0;
    ObjData d = {0};
    ObjObj *objs = NULL; int no = 0, co = 0;
    ObjObj *cur = NULL;
    char *line = txt;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        size_t ll = strlen(l); while (ll && (l[ll - 1] == '\r' || l[ll - 1] == ' ')) l[--ll] = 0;
        if (l[0] == 'o' && (l[1] == ' ' || l[1] == '\t')) {
            if (no == co) { co = co ? co * 2 : 16; objs = realloc(objs, sizeof(ObjObj) * co); }
            cur = &objs[no++]; memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", l + 2);
            cur->vstart = d.nv; cur->vend = d.nv;
        } else if (l[0] == 'v' && l[1] == ' ') {
            float x[3] = {0}; sscanf(l + 2, "%f %f %f", &x[0], &x[1], &x[2]);
            push3(&d.v, &d.nv, &d.cv, 3, x);
            if (cur) cur->vend = d.nv;
        } else if (l[0] == 'v' && l[1] == 't') {
            float x[2] = {0}; sscanf(l + 3, "%f %f", &x[0], &x[1]);
            push3(&d.vt, &d.nvt, &d.cvt, 2, x);
        } else if (l[0] == 'v' && l[1] == 'n') {
            float x[3] = {0}; sscanf(l + 3, "%f %f %f", &x[0], &x[1], &x[2]);
            push3(&d.vn, &d.nvn, &d.cvn, 3, x);
        } else if (l[0] == 'f' && l[1] == ' ' && cur) {
            char *tok = strtok(l + 2, " \t");
            while (tok) {
                int vi = 0, ti = 0, ni = 0;
                char *s1 = strchr(tok, '/');
                vi = atoi(tok);
                if (s1) {
                    if (s1[1] != '/') ti = atoi(s1 + 1);
                    char *s2 = strchr(s1 + 1, '/');
                    if (s2) ni = atoi(s2 + 1);
                }
                if (vi < 0) vi = d.nv + vi + 1;
                if (ti < 0) ti = d.nvt + ti + 1;
                if (ni < 0) ni = d.nvn + ni + 1;
                if (cur->nf == cur->cf) {
                    cur->cf = cur->cf ? cur->cf * 2 : 1024;
                    cur->fv = realloc(cur->fv, sizeof(int) * cur->cf);
                    cur->ft = realloc(cur->ft, sizeof(int) * cur->cf);
                    cur->fn = realloc(cur->fn, sizeof(int) * cur->cf);
                }
                cur->fv[cur->nf] = vi; cur->ft[cur->nf] = ti; cur->fn[cur->nf] = ni; cur->nf++;
                tok = strtok(NULL, " \t");
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    int changed_surfs = 0, bad = 0;
    for (int o = 0; o < no; o++) {
        ObjObj *ob = &objs[o];
        if (strstr(ob->name, "shared")) continue;
        char *sp = strstr(ob->name, "surf");
        if (!sp) continue;
        int si = atoi(sp + 4);
        if (si < 0 || si >= m->ns) continue;
        XSurf *s = &m->s[si];
        if (s->own[0] < 0) continue;
        int nv = ob->vend - ob->vstart;
        if (nv != s->vc) {
            err("%s: object %s has %d vertices but the game mesh has %d. Vertex count/order must stay the same (no adding/deleting/merging).", disp, ob->name, nv, s->vc);
            bad++; continue;
        }
        size_t v0 = (size_t)s->own[0];
        int vchg = 0, uvchg = 0, nchg = 0;
        for (int k = 0; k < nv; k++) {
            const float *P = d.v + 3 * (size_t)(ob->vstart + k);
            float nx = P[0], ny = -P[2], nz = P[1];
            float ox = xf32(v0 + 16 * k), oy = xf32(v0 + 16 * k + 4), oz = xf32(v0 + 16 * k + 8);
            float tol = 1e-4f;
            if (fabsf(nx - ox) > tol + 1e-6f * fabsf(ox) || fabsf(ny - oy) > tol + 1e-6f * fabsf(oy) || fabsf(nz - oz) > tol + 1e-6f * fabsf(oz)) {
                put_f32(z, v0 + 16 * k, nx); put_f32(z, v0 + 16 * k + 4, ny); put_f32(z, v0 + 16 * k + 8, nz);
                vchg++;
            }
        }
        if (s->own[1] >= 0 && ob->nf) {
            size_t v1 = (size_t)s->own[1];
            unsigned char *mk = calloc((size_t)nv, 1);
            for (int c = 0; c < ob->nf; c++) {
                int lv = ob->fv[c] - 1 - ob->vstart;
                if (lv < 0 || lv >= nv) continue;
                int ti = ob->ft[c], ni = ob->fn[c];
                if (ti > 0 && ti <= d.nvt) {
                    float u = d.vt[2 * (size_t)(ti - 1)], v = 1.0f - d.vt[2 * (size_t)(ti - 1) + 1];
                    float ou = half2f(xu16(v1 + 16 * lv + 4)), ov = half2f(xu16(v1 + 16 * lv + 6));
                    if (fabsf(u - ou) > 2e-4f || fabsf(v - ov) > 2e-4f) {
                        uint16_t hu = f2half(u), hv = f2half(v);
                        z[v1 + 16 * lv + 4] = hu >> 8; z[v1 + 16 * lv + 5] = hu & 255;
                        z[v1 + 16 * lv + 6] = hv >> 8; z[v1 + 16 * lv + 7] = hv & 255;
                        if (!(mk[lv] & 1)) { mk[lv] |= 1; uvchg++; }
                    }
                }
                if (ni > 0 && ni <= d.nvn) {
                    const float *N = d.vn + 3 * (size_t)(ni - 1);
                    float n[3] = { N[0], -N[2], N[1] };
                    float L = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (L > 1e-6f) {
                        n[0] /= L; n[1] /= L; n[2] /= L;
                        float o[3]; dec_normal(xu32(v1 + 16 * lv + 8), o);
                        float ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                        float dot = ol > 0 ? (n[0] * o[0] + n[1] * o[1] + n[2] * o[2]) / ol : 0;
                        if (dot < 0.9995f) { wbe32(z + v1 + 16 * lv + 8, enc_normal(n)); if (!(mk[lv] & 2)) { mk[lv] |= 2; nchg++; } }
                    }
                }
            }
            free(mk);
        }
        if (vchg || uvchg || nchg) {
            printf("  Model %s surf%02d: %d positions, %d UVs, %d normals changed\n", m->name, si, vchg, uvchg, nchg);
            changed_surfs++;
        }
    }
    for (int o = 0; o < no; o++) { free(objs[o].fv); free(objs[o].ft); free(objs[o].fn); }
    free(objs); free(d.v); free(d.vt); free(d.vn); free(txt);
    return bad ? -1 : changed_surfs;
}

/* returns number of changed surfaces, or -1 if any model failed */
static int models_import(unsigned char *z, size_t zl, const char *dir) {
    char *mdir = join(dir, "models");
    if (!u8isdir(mdir)) { free(mdir); return 0; }
    int nm;
    XModel *ms = scan_models(z, zl, &nm);
    int total = 0, failed = 0;
    for (int a = 0; a < nm; a++) {
        if (!ms[a].owns) continue;
        char fn[256], file[300];
        safe_name(ms[a].name, fn);
        snprintf(file, sizeof file, "%s.obj", fn);
        char *fp = join(mdir, file);
        if (u8exists(fp)) {
            int r = import_obj((unsigned char *)z, &ms[a], fp, file);
            if (r < 0) failed++; else total += r;
        }
        free(fp);
    }
    free_models(ms, nm);
    free(mdir);
    return failed ? -1 : total;
}
