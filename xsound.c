/*
 * xsound.c - LoadedSound (CoD WaW PS3) export to .mp3 and re-import.
 * Included from fftool.c after xtexture.c.
 *
 *   Record in the zone stream:
 *     u32 dataSize, u32 format (7 = MP3), u32 lengthMs, u32 channels, u32 sampleRate,
 *     u32 samples, u32 mp3Frames, ptr (-1), ptr name (-1), name\0, MP3 data[dataSize]
 *   Streamed sounds (music, battle chatter) are not in the .ff - they live in sound0.pak.
 */

typedef struct { size_t hdr, data; uint32_t size, ms, ch, rate, samples, frames; char name[220]; char file[260]; } XSound;

static int snd_rate_ok(uint32_t r) {
    return r == 8000 || r == 11025 || r == 12000 || r == 16000 || r == 22050 || r == 24000 || r == 32000 || r == 44100 || r == 48000;
}

static XSound *scan_sounds(const unsigned char *z, size_t zl, int *count) {
    int cap = 256, n = 0;
    XSound *s = malloc(sizeof(XSound) * cap);
    for (size_t p = 28; p + 12 < zl; p++) {
        if (z[p] != 0xFF || be32(z + p) != 0xFFFFFFFFu || be32(z + p + 4) != 0xFFFFFFFFu) continue;
        const unsigned char *h = z + p - 28;
        uint32_t size = be32(h), fmt = be32(h + 4), ch = be32(h + 12), rate = be32(h + 16);
        if (fmt != 7 || (ch != 1 && ch != 2) || !snd_rate_ok(rate) || size == 0 || size > 64u * 1024 * 1024) continue;
        size_t q = p + 8, i = 0;
        char nm[220];
        while (q + i < zl && i < sizeof nm - 1) {
            unsigned char c = z[q + i];
            if (!c) break;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '\\' || c == '.' || c == '-')) { i = 0; break; }
            nm[i++] = (char)c;
        }
        if (i < 2 || q + i >= zl || z[q + i]) continue;
        nm[i] = 0;
        size_t d = q + i + 1;
        if (d + size > zl) continue;
        if (!(z[d] == 0xFF && (z[d + 1] & 0xE0) == 0xE0) && memcmp(z + d, "ID3", 3)) continue;
        if (n == cap) { cap *= 2; s = realloc(s, sizeof(XSound) * cap); }
        XSound *x = &s[n++];
        memset(x, 0, sizeof *x);
        x->hdr = p - 28; x->data = d; x->size = size; x->ms = be32(h + 8); x->ch = ch; x->rate = rate;
        x->samples = be32(h + 20); x->frames = be32(h + 24);
        snprintf(x->name, sizeof x->name, "%s", nm);
        p = d + size - 1;
    }
    /* file names: keep folder structure, sanitize, de-duplicate */
    for (int k = 0; k < n; k++) {
        char tmp[240]; size_t j = 0;
        for (const char *c = s[k].name; *c && j < sizeof tmp - 8; c++) {
            char ch = *c;
            if (ch == '/' || ch == '\\') ch = SEP;
            else if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) ch = '_';
            tmp[j++] = ch;
        }
        tmp[j] = 0;
        if (strstr(tmp, "..")) { for (char *c = tmp; *c; c++) if (*c == '.') *c = '_'; }
        snprintf(s[k].file, sizeof s[k].file, "%s.mp3", tmp);
        int dup = 0;
        for (int m = 0; m < k; m++) if (!strcmp(s[m].file, s[k].file)) dup++;
        if (dup) snprintf(s[k].file, sizeof s[k].file, "%s_%d.mp3", tmp, dup + 1);
    }
    *count = n;
    return s;
}

/* parse an MP3 file: frames, samples, rate, channels, offset of first frame, end of last frame */
static int mp3_info(const unsigned char *d, size_t n, uint32_t *frames, uint32_t *samples, uint32_t *rate, uint32_t *ch, size_t *start, size_t *end) {
    static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int sr[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
    size_t p = 0;
    if (n >= 10 && !memcmp(d, "ID3", 3)) p = 10 + (((size_t)(d[6] & 127) << 21) | ((d[7] & 127) << 14) | ((d[8] & 127) << 7) | (d[9] & 127));
    *frames = 0; *samples = 0; *rate = 0; *ch = 0; *start = 0; *end = 0;
    while (p + 4 <= n) {
        if (d[p] != 0xFF || (d[p + 1] & 0xE0) != 0xE0) { if (*frames) break; p++; continue; }
        int ver = (d[p + 1] >> 3) & 3, layer = (d[p + 1] >> 1) & 3, bri = d[p + 2] >> 4, sri = (d[p + 2] >> 2) & 3, pad = (d[p + 2] >> 1) & 1, mode = d[p + 3] >> 6;
        if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) { if (*frames) break; p++; continue; }
        int vi = ver == 3 ? 0 : ver == 2 ? 1 : 2;
        int r = sr[vi][sri];
        int kbps = vi == 0 ? br1[bri] : br2[bri];
        int spf = vi == 0 ? 1152 : 576;
        size_t fl = (size_t)(spf / 8) * kbps * 1000 / r + pad;
        if (p + fl > n) break;
        if (!*frames) { *rate = r; *ch = mode == 3 ? 1 : 2; *start = p; }
        else if ((uint32_t)r != *rate) break;
        (*frames)++; *samples += spf; p += fl; *end = p;
    }
    return *frames > 0;
}

static int sounds_export(const unsigned char *z, size_t zl, const char *outdir) {
    int n;
    XSound *s = scan_sounds(z, zl, &n);
    if (!n) { free(s); return 0; }
    char *sdir = join(outdir, "sounds");
    u8mkdir(sdir);
    char *ip = join(sdir, "_index.txt");
    FILE *idx = u8fopen(ip, "wb");
    if (idx) fprintf(idx, "# FFTool sound list. file<TAB>bytes (max size)<TAB>rate<TAB>channels<TAB>seconds\r\n");
    int done = 0;
    for (int k = 0; k < n; k++) {
        char *fp = join(sdir, s[k].file);
        uint32_t wl = s[k].size;               /* drop zero padding left by an earlier replacement */
        { uint32_t f_, sm_, r_, c_; size_t st_, en_;
          if (mp3_info(z + s[k].data, s[k].size, &f_, &sm_, &r_, &c_, &st_, &en_) && en_ < wl) {
              size_t q = en_; while (q < wl && !z[s[k].data + q]) q++;
              if (q == wl) wl = (uint32_t)en_;
          } }
        if (write_file(fp, z + s[k].data, wl)) done++;
        else err("cannot write sounds\\%s", s[k].file);
        free(fp);
        if (idx) fprintf(idx, "%s\t%u\t%u\t%u\t%.2f\r\n", s[k].file, s[k].size, s[k].rate, s[k].ch, s[k].rate ? (double)s[k].samples / s[k].rate : 0.0);
    }
    if (idx) fclose(idx);
    free(ip); free(sdir); free(s);
    return done;
}

/* returns changed count or -1 on error */
static int sounds_import(unsigned char *z, size_t zl, const char *dir) {
    char *sdir = join(dir, "sounds");
    if (!u8isdir(sdir)) { free(sdir); return 0; }
    int n;
    XSound *s = scan_sounds(z, zl, &n);
    int changed = 0, bad = 0;
    for (int k = 0; k < n; k++) {
        char *fp = join(sdir, s[k].file);
        size_t len;
        unsigned char *d = read_file(fp, &len);
        free(fp);
        if (!d) continue;
        if (len <= s[k].size && !memcmp(d, z + s[k].data, len)) {
            size_t q = len; while (q < s[k].size && !z[s[k].data + q]) q++;
            if (q == s[k].size) { free(d); continue; }   /* unchanged (or trimmed copy of current data) */
        }
        uint32_t fr, sm, rt, ch; size_t st, en;
        if (!mp3_info(d, len, &fr, &sm, &rt, &ch, &st, &en)) {
            err("sounds\\%s is not an MP3 file (export it as MP3, %u Hz)", s[k].file, s[k].rate); bad++; free(d); continue;
        }
        size_t need = en - st;            /* frames only - ID3 tags are dropped */
        if (need > s[k].size) {
            err("sounds\\%s is %zu bytes too big (max %u bytes). Use a lower bitrate (e.g. 64-96 kbps), mono, or shorten it.",
                s[k].file, need - s[k].size, s[k].size);
            bad++; free(d); continue;
        }
        unsigned char *dst = z + s[k].data;
        memcpy(dst, d + st, need);
        memset(dst + need, 0, s[k].size - need);
        unsigned char *h = z + s[k].hdr;
        wbe32(h + 8, (uint32_t)((uint64_t)sm * 1000 / rt));
        wbe32(h + 12, ch);
        wbe32(h + 16, rt);
        wbe32(h + 20, sm);
        wbe32(h + 24, fr);
        printf("  Sound %s: replaced (%.2fs, %u Hz, %s, %zu/%u bytes)\n", s[k].file, (double)sm / rt, rt, ch == 1 ? "mono" : "stereo", need, s[k].size);
        changed++;
        free(d);
    }
    free(s); free(sdir);
    return bad ? -1 : changed;
}
