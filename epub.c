/*
 * epub.c - a tiny terminal EPUB reader.
 *
 * Build:  cc -O2 -Wall -o epub epub.c -lz
 * Use:    ./epub book.epub            (interactive)
 *         ./epub book.epub --dump     (print whole book as plain text)
 *
 * How it works:
 *   1. An EPUB is a ZIP archive. We parse the ZIP central directory ourselves
 *      and inflate entries with zlib (raw deflate).
 *   2. META-INF/container.xml points to the OPF package file.
 *   3. The OPF <spine> lists the chapters (XHTML) in reading order.
 *   4. Each chapter is converted HTML -> text, word-wrapped, and paged.
 *
 * Keys: j/Down/Enter line down   k/Up line up      Space/PgDn page down
 *       b/PgUp page up           n/Right next ch.  p/Left prev chapter
 *       g/Home top               G/End bottom      q quit
 *
 * Limits: no ZIP64, no encryption, no images, no CSS. Text only.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <zlib.h>

/* ------------------------------------------------------------ utilities */

static void die(const char *m) { fprintf(stderr, "epub: %s\n", m); exit(1); }

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static uint32_t u16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------ ZIP reader */

typedef struct { char *name; uint32_t method, csize, usize, offset; } ZEntry;
typedef struct { unsigned char *data; size_t size; ZEntry *e; int n; } Zip;

static void zip_open(Zip *z, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 22) die("not a zip file");
    z->data = xrealloc(NULL, (size_t)n);
    if (fread(z->data, 1, (size_t)n, f) != (size_t)n) die("read error");
    fclose(f);
    z->size = (size_t)n;

    /* find End Of Central Directory record (scan backwards over the comment) */
    long i = n - 22, lim = n - 22 - 65535;
    for (; i >= 0 && i >= lim; i--)
        if (u32(z->data + i) == 0x06054b50) break;
    if (i < 0 || u32(z->data + i) != 0x06054b50) die("not a zip file (no end record)");

    int cnt = (int)u16(z->data + i + 10);
    size_t off = u32(z->data + i + 16);
    z->e = xrealloc(NULL, sizeof(ZEntry) * (size_t)(cnt ? cnt : 1));
    z->n = 0;
    for (int k = 0; k < cnt; k++) {
        if (off + 46 > z->size || u32(z->data + off) != 0x02014b50) break;
        const unsigned char *c = z->data + off;
        size_t nl = u16(c + 28), el = u16(c + 30), cl = u16(c + 32);
        if (off + 46 + nl > z->size) break;
        ZEntry *e = &z->e[z->n++];
        e->method = u16(c + 10);
        e->csize = u32(c + 20);
        e->usize = u32(c + 24);
        e->offset = u32(c + 42);
        e->name = xrealloc(NULL, nl + 1);
        memcpy(e->name, c + 46, nl);
        e->name[nl] = 0;
        off += 46 + nl + el + cl;
    }
}

/* Returns a NUL-terminated malloc'd buffer, or NULL if missing/corrupt. */
static char *zip_read(Zip *z, const char *name) {
    for (int i = 0; i < z->n; i++) {
        if (strcmp(z->e[i].name, name) != 0) continue;
        ZEntry *e = &z->e[i];
        if ((size_t)e->offset + 30 > z->size) return NULL;
        const unsigned char *l = z->data + e->offset;
        if (u32(l) != 0x04034b50) return NULL;
        size_t start = (size_t)e->offset + 30 + u16(l + 26) + u16(l + 28);
        if (start + e->csize > z->size) return NULL;

        char *out = xrealloc(NULL, (size_t)e->usize + 1);
        out[e->usize] = 0;
        if (e->method == 0) {
            if (e->csize != e->usize) { free(out); return NULL; }
            memcpy(out, z->data + start, e->usize);
        } else if (e->method == 8) {
            z_stream s;
            memset(&s, 0, sizeof s);
            if (inflateInit2(&s, -15) != Z_OK) { free(out); return NULL; }
            s.next_in = (Bytef *)(z->data + start);
            s.avail_in = e->csize;
            s.next_out = (Bytef *)out;
            s.avail_out = e->usize;
            int r = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (r != Z_STREAM_END) { free(out); return NULL; }
        } else {
            free(out);
            return NULL;
        }
        return out;
    }
    return NULL;
}

/* ------------------------------------------------------------ tiny XML helpers */

/* Copy the next <...> (without brackets) into buf and advance *p. */
static int next_tag(const char **p, char *buf, size_t n) {
    const char *s = strchr(*p, '<');
    if (!s) return 0;
    const char *e = strchr(s, '>');
    if (!e) return 0;
    size_t len = (size_t)(e - s - 1);
    if (len >= n) len = n - 1;
    memcpy(buf, s + 1, len);
    buf[len] = 0;
    *p = e + 1;
    return 1;
}

/* Fetch attribute `name` from a tag string. */
static int attr(const char *tag, const char *name, char *out, size_t n) {
    size_t ln = strlen(name);
    for (const char *p = tag; *p; p++) {
        if (p == tag || !isspace((unsigned char)p[-1]) || strncmp(p, name, ln) != 0) continue;
        const char *q = p + ln;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '=') continue;
        q++;
        while (isspace((unsigned char)*q)) q++;
        char quote = *q;
        if (quote != '"' && quote != '\'') continue;
        q++;
        size_t i = 0;
        while (*q && *q != quote && i + 1 < n) out[i++] = *q++;
        out[i] = 0;
        return 1;
    }
    return 0;
}

static int tag_is(const char *tag, const char *name) {
    size_t l = strlen(name);
    return strncmp(tag, name, l) == 0 && (isspace((unsigned char)tag[l]) || tag[l] == 0 || tag[l] == '/');
}

static void url_decode(char *s) {
    char *w = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *w++ = *s;
        }
    }
    *w = 0;
}

/* dir + href with "." / ".." segments resolved, fragment removed. */
static void resolve(const char *dir, const char *href, char *out, size_t n) {
    char tmp[1024], *parts[128];
    int np = 0;
    snprintf(tmp, sizeof tmp, "%s%s", dir, href);
    char *hash = strchr(tmp, '#');
    if (hash) *hash = 0;
    for (char *t = strtok(tmp, "/"); t && np < 128; t = strtok(NULL, "/")) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        parts[np++] = t;
    }
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        if (i) strncat(out, "/", n - strlen(out) - 1);
        strncat(out, parts[i], n - strlen(out) - 1);
    }
}

/* ------------------------------------------------------------ book model */

static char *html_to_text(const char *h);

typedef struct { char id[128], href[512]; int is_html; } Item;
typedef struct {
    Zip z;
    char title[256];
    char **spine;   /* resolved paths inside the zip, in reading order */
    int nspine;
} Book;

static void book_open(Book *b, const char *path) {
    memset(b, 0, sizeof *b);
    zip_open(&b->z, path);

    char *cont = zip_read(&b->z, "META-INF/container.xml");
    if (!cont) die("missing META-INF/container.xml (not an EPUB?)");
    char tag[2048], opf_path[512] = "";
    const char *p = cont;
    while (next_tag(&p, tag, sizeof tag))
        if (tag_is(tag, "rootfile") && attr(tag, "full-path", opf_path, sizeof opf_path)) break;
    free(cont);
    if (!opf_path[0]) die("no rootfile in container.xml");
    url_decode(opf_path);

    char *opf = zip_read(&b->z, opf_path);
    if (!opf) die("cannot read OPF package file");

    char dir[512];
    snprintf(dir, sizeof dir, "%s", opf_path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;

    /* title */
    snprintf(b->title, sizeof b->title, "(untitled)");
    const char *t = strstr(opf, "<dc:title");
    if (t && (t = strchr(t, '>'))) {
        t++;
        const char *e = strchr(t, '<');
        if (e && e > t) {
            size_t len = (size_t)(e - t);
            char *raw = xrealloc(NULL, len + 1);
            memcpy(raw, t, len);
            raw[len] = 0;
            char *dec = html_to_text(raw);      /* decode entities, collapse spaces */
            if (*dec) snprintf(b->title, sizeof b->title, "%s", dec);
            free(dec);
            free(raw);
        }
    }

    /* manifest + spine */
    Item *items = NULL;
    int nitems = 0, cap = 0;
    p = opf;
    while (next_tag(&p, tag, sizeof tag)) {
        if (tag_is(tag, "item")) {
            if (nitems == cap) { cap = cap ? cap * 2 : 32; items = xrealloc(items, sizeof(Item) * (size_t)cap); }
            Item *it = &items[nitems];
            char mt[64] = "";
            it->id[0] = it->href[0] = 0;
            attr(tag, "id", it->id, sizeof it->id);
            attr(tag, "href", it->href, sizeof it->href);
            attr(tag, "media-type", mt, sizeof mt);
            url_decode(it->href);
            it->is_html = strstr(mt, "html") != NULL;
            nitems++;
        } else if (tag_is(tag, "itemref")) {
            char idref[128];
            if (!attr(tag, "idref", idref, sizeof idref)) continue;
            for (int i = 0; i < nitems; i++) {
                if (strcmp(items[i].id, idref) != 0 || !items[i].is_html) continue;
                char full[1024];
                resolve(dir, items[i].href, full, sizeof full);
                b->spine = xrealloc(b->spine, sizeof(char *) * (size_t)(b->nspine + 1));
                size_t fl = strlen(full) + 1;
                b->spine[b->nspine] = memcpy(xrealloc(NULL, fl), full, fl);
                b->nspine++;
                break;
            }
        }
    }
    free(items);
    free(opf);
    if (b->nspine == 0) die("book has no readable chapters");
}

/* ------------------------------------------------------------ HTML -> text */

typedef struct { char *s; size_t n, cap; } Buf;

static void bput(Buf *b, char c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 4096; b->s = xrealloc(b->s, b->cap); }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}
static void strip_spaces(Buf *b) { while (b->n && b->s[b->n - 1] == ' ') b->s[--b->n] = 0; }

/* text char with whitespace collapsing */
static void put_text(Buf *b, char c) {
    if (isspace((unsigned char)c)) {
        if (b->n && b->s[b->n - 1] != ' ' && b->s[b->n - 1] != '\n') bput(b, ' ');
    } else {
        bput(b, c);
    }
}

static void put_cp(Buf *b, unsigned cp) {
    if (cp == 0xA0) cp = ' ';
    if (cp < 0x80) { put_text(b, (char)cp); return; }
    if (cp < 0x800) {
        bput(b, (char)(0xC0 | (cp >> 6)));
        bput(b, (char)(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        bput(b, (char)(0xE0 | (cp >> 12)));
        bput(b, (char)(0x80 | ((cp >> 6) & 63)));
        bput(b, (char)(0x80 | (cp & 63)));
    } else {
        bput(b, (char)(0xF0 | (cp >> 18)));
        bput(b, (char)(0x80 | ((cp >> 12) & 63)));
        bput(b, (char)(0x80 | ((cp >> 6) & 63)));
        bput(b, (char)(0x80 | (cp & 63)));
    }
}

static void para(Buf *b) {            /* blank line between blocks */
    strip_spaces(b);
    if (!b->n) return;
    if (b->n >= 2 && b->s[b->n - 1] == '\n' && b->s[b->n - 2] == '\n') return;
    if (b->s[b->n - 1] != '\n') bput(b, '\n');
    bput(b, '\n');
}
static void newline(Buf *b) {         /* soft line break */
    strip_spaces(b);
    if (!b->n || b->s[b->n - 1] == '\n') return;
    bput(b, '\n');
}
static void hard_break(Buf *b) {      /* <br> */
    strip_spaces(b);
    if (b->n >= 2 && b->s[b->n - 1] == '\n' && b->s[b->n - 2] == '\n') return;
    bput(b, '\n');
}

static unsigned named_entity(const char *e) {
    static const struct { const char *n; unsigned cp; } T[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", 0xA0}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"copy", 0xA9}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"eacute", 0xE9},
        {"egrave", 0xE8}, {"agrave", 0xE0}, {"ccedil", 0xE7}, {"ecirc", 0xEA},
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (!strcmp(T[i].n, e)) return T[i].cp;
    return 0;
}

static int is_block(const char *n) {
    static const char *B[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
                              "section", "article", "header", "footer", "ul", "ol", "table",
                              "tr", "pre", "figure", "hr", "aside", "dl", "dt", "dd"};
    for (size_t i = 0; i < sizeof B / sizeof *B; i++)
        if (!strcmp(n, B[i])) return 1;
    return 0;
}

static char *html_to_text(const char *h) {
    Buf b = {0};
    char skip[16] = "";
    while (*h) {
        if (*h == '<') {
            if (!strncmp(h, "<!--", 4)) {
                const char *e = strstr(h + 4, "-->");
                if (!e) break;
                h = e + 3;
                continue;
            }
            const char *e = strchr(h, '>');
            if (!e) break;
            const char *t = h + 1;
            int close = 0, i = 0;
            char name[16];
            if (*t == '/') { close = 1; t++; }
            while (t < e && (isalnum((unsigned char)*t) || *t == ':') && i < 15)
                name[i++] = (char)tolower((unsigned char)*t++);
            name[i] = 0;
            int selfclose = e[-1] == '/';

            if (skip[0]) {
                if (close && !strcmp(name, skip)) skip[0] = 0;
            } else if (!close && !selfclose &&
                       (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "head"))) {
                strcpy(skip, name);
            } else if (!strcmp(name, "br")) {
                hard_break(&b);
            } else if (!strcmp(name, "li")) {
                newline(&b);
                if (!close) { bput(&b, '\xE2'); bput(&b, '\x80'); bput(&b, '\xA2'); bput(&b, ' '); }
            } else if (is_block(name)) {
                para(&b);
            }
            h = e + 1;
        } else if (*h == '&') {
            const char *e = strchr(h, ';');
            unsigned cp = 0;
            if (e && e - h <= 10) {
                char ent[12];
                memcpy(ent, h + 1, (size_t)(e - h - 1));
                ent[e - h - 1] = 0;
                if (ent[0] == '#')
                    cp = (unsigned)((ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent + 2, NULL, 16)
                                                                     : strtoul(ent + 1, NULL, 10));
                else
                    cp = named_entity(ent);
            }
            if (cp) { if (!skip[0]) put_cp(&b, cp); h = e + 1; }
            else    { if (!skip[0]) put_text(&b, '&'); h++; }
        } else {
            if (!skip[0]) put_text(&b, *h);
            h++;
        }
    }
    while (b.n && (b.s[b.n - 1] == '\n' || b.s[b.n - 1] == ' ')) b.s[--b.n] = 0;
    if (!b.s) { b.s = xrealloc(NULL, 1); b.s[0] = 0; }
    return b.s;
}

/* ------------------------------------------------------------ word wrap */

typedef struct { char **v; int n, cap; } Lines;

static void lput(Lines *L, const char *s, size_t len) {
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 256; L->v = xrealloc(L->v, sizeof(char *) * (size_t)L->cap); }
    char *d = xrealloc(NULL, len + 1);
    memcpy(d, s, len);
    d[len] = 0;
    L->v[L->n++] = d;
}
static void lfree(Lines *L) {
    for (int i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
    memset(L, 0, sizeof *L);
}

static size_t cplen(const char *s) {   /* bytes in the UTF-8 char starting at s */
    unsigned char c = (unsigned char)*s;
    return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

/* Greedy word wrap; width is counted in Unicode code points. */
static void wrap(const char *text, int width, Lines *L) {
    const char *p = text;
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        if (end == p) {
            lput(L, "", 0);
        } else {
            const char *ls = p, *q = p;
            int col = 0;
            while (q < end) {
                const char *ws = q;
                while (ws < end && *ws == ' ') ws++;
                const char *we = ws;
                int wc = 0;
                while (we < end && *we != ' ') { we += cplen(we); wc++; }
                if (we > end) we = end;
                if (ws == we) break;
                int sp = col > 0;
                if (col + sp + wc <= width) {
                    if (col == 0) ls = ws;
                    col += sp + wc;
                    q = we;
                } else if (col > 0) {
                    lput(L, ls, (size_t)(q - ls));
                    ls = q = ws;
                    col = 0;
                } else {                       /* single word wider than the page */
                    const char *c = ws;
                    for (int k = 0; c < we && k < width; k++) c += cplen(c);
                    if (c > we) c = we;
                    lput(L, ws, (size_t)(c - ws));
                    ls = q = c;
                }
            }
            if (col > 0) lput(L, ls, (size_t)(q - ls));
        }
        if (!eol) break;
        p = eol + 1;
    }
}

/* ------------------------------------------------------------ terminal */

enum { K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_PGUP, K_PGDN, K_HOME, K_END };

static struct termios g_orig;
static volatile sig_atomic_t g_quit;

static void term_restore(void) {
    tcsetattr(0, TCSAFLUSH, &g_orig);
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}
static void term_raw(void) {
    tcgetattr(0, &g_orig);
    atexit(term_restore);
    struct termios r = g_orig;
    r.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    r.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    r.c_cc[VMIN] = 1;
    r.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &r);
    fputs("\033[?1049h\033[?25l", stdout);
}
static void on_signal(int s) { if (s != SIGWINCH) g_quit = 1; }   /* just interrupts read() */
static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;            /* no SA_RESTART: read() returns EINTR */
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
static void term_size(int *rows, int *cols) {
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) { *rows = w.ws_row; *cols = w.ws_col; }
    else { *rows = 24; *cols = 80; }
}
static int input_ready(int ms) { struct pollfd pf = {0, POLLIN, 0}; return poll(&pf, 1, ms) > 0; }

static int read_key(void) {
    unsigned char c;
    if (read(0, &c, 1) <= 0) return -1;           /* interrupted (resize / signal) */
    if (c != 27) return c;
    if (!input_ready(40)) return 27;
    unsigned char c2, c3, c4;
    if (read(0, &c2, 1) <= 0) return 27;
    if (c2 != '[' && c2 != 'O') return 27;
    if (read(0, &c3, 1) <= 0) return 27;
    switch (c3) {
    case 'A': return K_UP;    case 'B': return K_DOWN;
    case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME;  case 'F': return K_END;
    }
    if (isdigit(c3) && input_ready(40) && read(0, &c4, 1) > 0 && c4 == '~') {
        switch (c3) {
        case '5': return K_PGUP;
        case '6': return K_PGDN;
        case '1': case '7': return K_HOME;
        case '4': case '8': return K_END;
        }
    }
    return 27;
}

/* print at most maxcp code points of s */
static void put_trunc(const char *s, int maxcp) {
    for (int k = 0; *s && k < maxcp; k++) {
        size_t l = cplen(s);
        fwrite(s, 1, l, stdout);
        s += l;
    }
}
static int cp_count(const char *s) { int n = 0; for (; *s; s += cplen(s)) n++; return n; }

/* ------------------------------------------------------------ reader state */

static Book g_book;
static int g_ch = -1, g_top, g_width, g_ph = 1;
static char *g_text = NULL;
static Lines g_lines;

static void relayout(int width) {
    int oldn = g_lines.n;
    double frac = oldn > 0 ? (double)g_top / oldn : 0;
    lfree(&g_lines);
    wrap(g_text, width, &g_lines);
    g_width = width;
    g_top = (int)(frac * g_lines.n);
}

static void load_chapter(int i, int width) {
    free(g_text);
    char *html = zip_read(&g_book.z, g_book.spine[i]);
    g_text = html ? html_to_text(html) : strdup("[could not read this chapter]");
    free(html);
    if (!g_text) die("out of memory");
    g_ch = i;
    g_top = 0;
    lfree(&g_lines);
    wrap(g_text, width, &g_lines);
    g_width = width;
}

static void draw(void) {
    int rows, cols;
    term_size(&rows, &cols);
    int w = cols - 4;
    if (w > 80) w = 80;
    if (w < 10) w = 10;
    if (w != g_width) relayout(w);
    int margin = (cols - w) / 2;
    if (margin < 0) margin = 0;
    int ph = rows - 2;
    if (ph < 1) ph = 1;
    g_ph = ph;

    int maxtop = g_lines.n - ph;
    if (maxtop < 0) maxtop = 0;
    if (g_top > maxtop) g_top = maxtop;
    if (g_top < 0) g_top = 0;

    fputs("\033[H", stdout);
    for (int r = 0; r < ph; r++) {
        printf("\033[%d;1H\033[2K", r + 1);
        int idx = g_top + r;
        if (idx < g_lines.n) printf("%*s%s", margin, "", g_lines.v[idx]);
    }
    int pct = g_lines.n ? (g_top + ph >= g_lines.n ? 100 : (g_top + ph) * 100 / g_lines.n) : 100;
    char status[600];
    snprintf(status, sizeof status, " %s  |  ch %d/%d  |  %d%%  |  q quit  n/p chapter ",
             g_book.title, g_ch + 1, g_book.nspine, pct);
    printf("\033[%d;1H\033[7m", rows);
    put_trunc(status, cols);
    for (int k = cp_count(status); k < cols; k++) fputc(' ', stdout);
    fputs("\033[0m", stdout);
    fflush(stdout);
}

static void goto_chapter(int i, int at_end) {
    if (i < 0 || i >= g_book.nspine) return;
    load_chapter(i, g_width ? g_width : 72);
    if (at_end) g_top = 1 << 30;   /* draw() clamps to the real bottom */
}

static void run_tui(void) {
    term_raw();
    install_signals();
    int rows, cols;
    term_size(&rows, &cols);
    load_chapter(0, 72);
    while (!g_quit) {
        draw();
        int k = read_key();
        if (g_quit) break;
        if (k < 0) continue;
        int n = g_lines.n, step = g_ph > 1 ? g_ph - 1 : 1;
        switch (k) {
        case 'q': case 'Q': g_quit = 1; break;
        case 'j': case K_DOWN: case '\r': case '\n': g_top++; break;
        case 'k': case K_UP: g_top--; break;
        case ' ': case 'f': case K_PGDN:
            if (g_top + g_ph >= n) goto_chapter(g_ch + 1, 0); else g_top += step;
            break;
        case 'b': case K_PGUP:
            if (g_top <= 0) goto_chapter(g_ch - 1, 1); else g_top -= step;
            break;
        case 'n': case K_RIGHT: goto_chapter(g_ch + 1, 0); break;
        case 'p': case K_LEFT:  goto_chapter(g_ch - 1, 0); break;
        case 'g': case K_HOME:  g_top = 0; break;
        case 'G': case K_END:   g_top = 1 << 30; break;
        }
    }
}

static void run_dump(void) {
    for (int i = 0; i < g_book.nspine; i++) {
        char *html = zip_read(&g_book.z, g_book.spine[i]);
        if (!html) continue;
        char *text = html_to_text(html);
        Lines L = {0};
        wrap(text, 80, &L);
        printf("\n===== [%d/%d] %s =====\n\n", i + 1, g_book.nspine, g_book.spine[i]);
        for (int k = 0; k < L.n; k++) puts(L.v[k]);
        lfree(&L);
        free(text);
        free(html);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s book.epub [--dump]\n", argv[0]);
        return 2;
    }
    book_open(&g_book, argv[1]);
    if (argc > 2 && !strcmp(argv[2], "--dump")) { run_dump(); return 0; }
    if (!isatty(0) || !isatty(1)) die("interactive mode needs a terminal (try --dump)");
    run_tui();
    return 0;
}
