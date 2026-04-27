#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>

#define PROGRAM_NAME "base"
#define VERSION "1.0"

/* I/O buffer sizes — large enough to amortize syscall overhead */
#define IN_BUF_SIZE  (1 << 17)   /* 128 KiB read chunks */
#define OUT_BUF_SIZE (1 << 18)   /* 256 KiB write buffer */

static void usage(int status);
static void version(void);

typedef struct {
    int base;      /* 2, 8, 10, 16, or 0 for raw */
    int endian;    /* 'b' for BE, 'l' for LE */
} basespec;

/* ── error helpers ───────────────────────────────────────────────────────── */

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", PROGRAM_NAME, msg);
    fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    exit(EXIT_FAILURE);
}

static void die_fmt(const char *fmt, const char *arg)
{
    fprintf(stderr, "%s: ", PROGRAM_NAME);
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    exit(EXIT_FAILURE);
}

/* ── lookup tables ───────────────────────────────────────────────────────── */

/* hex nibble → ASCII char */
static const char HEX_ENC[16] = {
    '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};

/* byte → two hex ASCII chars packed in a uint16_t (lo=first char, hi=second) */
static uint16_t HEX_PAIR[256];

/* byte → 8 binary ASCII chars (stored as char[8]) */
static char BIN_BYTE[256][8];

/* byte → 3 octal ASCII chars (stored as char[3]) */
static char OCT_BYTE[256][3];

/* ASCII hex char → nibble value (0xff = invalid) */
static uint8_t HEX_DEC[256];

/* ASCII octal char → value, or 0xff = invalid */
static uint8_t OCT_DEC[256];

/* ASCII binary char → value, or 0xff = invalid */
static uint8_t BIN_DEC[256];

static void init_tables(void)
{
    /* hex encode */
    for (int i = 0; i < 256; i++) {
        HEX_PAIR[i] = (uint16_t)HEX_ENC[i & 0xf] << 8 | HEX_ENC[i >> 4];
        /* note: stored lo=high-nibble-char, hi=low-nibble-char so we can
           cast &HEX_PAIR[b] to char* and write 2 bytes in order */
        HEX_PAIR[i] = (uint16_t)(HEX_ENC[i >> 4]) | ((uint16_t)(HEX_ENC[i & 0xf]) << 8);
    }

    /* binary encode */
    for (int i = 0; i < 256; i++)
        for (int b = 7; b >= 0; b--)
            BIN_BYTE[i][7 - b] = '0' + ((i >> b) & 1);

    /* octal encode */
    for (int i = 0; i < 256; i++) {
        OCT_BYTE[i][0] = '0' + ((i >> 6) & 7);
        OCT_BYTE[i][1] = '0' + ((i >> 3) & 7);
        OCT_BYTE[i][2] = '0' + ( i       & 7);
    }

    /* hex decode: branchless nibble decode via bit trick
       for '0'-'9': (c & 0xf) = digit value
       for 'a'-'f': (c & 0xf) = 1-6, +9 = 10-15
       for 'A'-'F': same
       trick: val = (c & 0xf) + (c >> 6) * 9  */
    memset(HEX_DEC, 0xff, sizeof(HEX_DEC));
    for (int c = 0; c < 256; c++) {
        if (c >= '0' && c <= '9') HEX_DEC[c] = c - '0';
        else if (c >= 'a' && c <= 'f') HEX_DEC[c] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') HEX_DEC[c] = c - 'A' + 10;
    }

    /* octal decode */
    memset(OCT_DEC, 0xff, sizeof(OCT_DEC));
    for (int c = '0'; c <= '7'; c++) OCT_DEC[c] = c - '0';

    /* binary decode */
    memset(BIN_DEC, 0xff, sizeof(BIN_DEC));
    BIN_DEC[(unsigned)'0'] = 0;
    BIN_DEC[(unsigned)'1'] = 1;
}

/* ── output buffer ───────────────────────────────────────────────────────── */

static char   obuf[OUT_BUF_SIZE];
static size_t opos = 0;

static inline void obuf_flush(void)
{
    if (opos) {
        fwrite(obuf, 1, opos, stdout);
        opos = 0;
    }
}

static inline void obuf_write(const char *src, size_t n)
{
    if (opos + n > OUT_BUF_SIZE) obuf_flush();
    /* for very large writes bypass buffer */
    if (n >= OUT_BUF_SIZE) { fwrite(src, 1, n, stdout); return; }
    memcpy(obuf + opos, src, n);
    opos += n;
}

static inline void obuf_putc(char c)
{
    if (opos >= OUT_BUF_SIZE) obuf_flush();
    obuf[opos++] = c;
}

/* ── parse_base ──────────────────────────────────────────────────────────── */

static int parse_base(const char *s, basespec *out)
{
    int base;
    const char *rest;

    if      (strncmp(s, "16", 2) == 0) { base = 16; rest = s + 2; }
    else if (strncmp(s, "10", 2) == 0) { base = 10; rest = s + 2; }
    else if (strncmp(s, "8",  1) == 0) { base = 8;  rest = s + 1; }
    else if (strncmp(s, "2",  1) == 0) { base = 2;  rest = s + 1; }
    else return -1;

    int endian = 'b';
    if      (*rest == '\0')          endian = 'b';
    else if (strcmp(rest, "be") == 0) endian = 'b';
    else if (strcmp(rest, "le") == 0) endian = 'l';
    else return -1;

    out->base   = base;
    out->endian = endian;
    return 0;
}

/* ── dynamic stdin reader (for paths that need full buffer) ──────────────── */

static unsigned char *read_stdin_full(size_t *out_len)
{
    size_t cap = IN_BUF_SIZE;
    size_t len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            unsigned char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
            buf = tmp;
        }
    }
    if (ferror(stdin)) { free(buf); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    *out_len = len;
    return buf;
}

/* ── streaming encode (raw → base 2/8/16) ───────────────────────────────── */

static void stream_encode(basespec spec)
{
    unsigned char ibuf[IN_BUF_SIZE];
    size_t n;

    if (spec.endian == 'b') {
        while ((n = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0) {
            for (size_t i = 0; i < n; i++) {
                unsigned char b = ibuf[i];
                switch (spec.base) {
                    case 16: obuf_write((char *)&HEX_PAIR[b], 2); break;
                    case 8:  obuf_write(OCT_BYTE[b], 3); break;
                    case 2:  obuf_write(BIN_BYTE[b], 8); break;
                }
            }
        }
    } else {
        /* LE: need whole input to reverse */
        size_t len;
        unsigned char *buf = read_stdin_full(&len);
        for (size_t i = len; i > 0; i--) {
            unsigned char b = buf[i - 1];
            switch (spec.base) {
                case 16: obuf_write((char *)&HEX_PAIR[b], 2); break;
                case 8:  obuf_write(OCT_BYTE[b], 3); break;
                case 2:  obuf_write(BIN_BYTE[b], 8); break;
            }
        }
        free(buf);
    }

    if (ferror(stdin)) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    obuf_flush();
}

/* ── streaming decode (base 2/8/16 → raw) ───────────────────────────────── */

static void stream_decode(basespec spec)
{
    /* digits per output byte */
    int dpb = (spec.base == 2) ? 8 : (spec.base == 8) ? 3 : 2;
    const uint8_t *dtab = (spec.base == 2) ? BIN_DEC :
                          (spec.base == 8) ? OCT_DEC : HEX_DEC;

    unsigned char ibuf[IN_BUF_SIZE];

    /* accumulator: collect dpb valid digits then emit a byte */
    unsigned acc = 0;
    int acc_bits = 0;   /* bits accumulated so far (log2 of partial value) */
    int shift    = (spec.base == 2) ? 1 : (spec.base == 8) ? 3 : 4;
    int acc_need = dpb * shift; /* total bits in a full byte */

    /* collect all output bytes (needed for LE reversal) */
    unsigned char *collected = NULL;
    size_t         clen = 0, ccap = 0;
    int need_collect = (spec.endian == 'l');

    if (need_collect) {
        ccap = IN_BUF_SIZE;
        collected = malloc(ccap);
        if (!collected) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    }

    size_t n;
    while ((n = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0) {
        for (size_t i = 0; i < n; i++) {
            uint8_t d = dtab[(unsigned char)ibuf[i]];
            if (d == 0xff) continue; /* whitespace / ignored */

            acc = (acc << shift) | d;
            acc_bits += shift;

            if (acc_bits == acc_need) {
                unsigned char byte = (unsigned char)(acc & 0xff);
                if (need_collect) {
                    if (clen >= ccap) {
                        ccap *= 2;
                        unsigned char *tmp = realloc(collected, ccap);
                        if (!tmp) { free(collected); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
                        collected = tmp;
                    }
                    collected[clen++] = byte;
                } else {
                    obuf_putc((char)byte);
                }
                acc = 0;
                acc_bits = 0;
            }
        }
    }

    if (ferror(stdin)) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }

    if (need_collect) {
        /* reverse for LE */
        for (size_t i = clen; i > 0; i--)
            obuf_putc((char)collected[i - 1]);
        free(collected);
    }

    obuf_flush();
}

/* ── base10 encode (bignum, arbitrary size) ──────────────────────────────── */

static void encode_base10(const unsigned char *buf, size_t len, int endian)
{
    if (len == 0) die("empty input");

    size_t maxdigits = (size_t)(len * 2.41) + 4;
    uint8_t *digits = calloc(maxdigits, 1);
    if (!digits) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t ndigits = 1;

    for (size_t bi = 0; bi < len; bi++) {
        unsigned byte = (endian == 'b') ? buf[bi] : buf[len - 1 - bi];

        unsigned carry = 0;
        for (size_t i = 0; i < ndigits; i++) {
            unsigned v = digits[i] * 256 + carry;
            digits[i] = v % 10;
            carry = v / 10;
        }
        while (carry) {
            if (ndigits >= maxdigits) { free(digits); die("bignum overflow"); }
            digits[ndigits++] = carry % 10;
            carry /= 10;
        }

        carry = byte;
        for (size_t i = 0; i < ndigits && carry; i++) {
            unsigned v = digits[i] + carry;
            digits[i] = v % 10;
            carry = v / 10;
        }
        while (carry) {
            if (ndigits >= maxdigits) { free(digits); die("bignum overflow"); }
            digits[ndigits++] = carry % 10;
            carry /= 10;
        }
    }

    for (size_t i = ndigits; i > 0; i--)
        obuf_putc('0' + digits[i - 1]);

    obuf_flush();
    free(digits);
}

/* ── base10 decode (bignum, arbitrary size) ──────────────────────────────── */

static unsigned char *decode_base10(const unsigned char *in, size_t in_len,
                                     int endian, size_t *out_len)
{
    /* strip whitespace inline */
    char *clean = malloc(in_len + 1);
    if (!clean) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t dlen = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (isspace((unsigned char)in[i])) continue;
        if (in[i] < '0' || in[i] > '9') {
            free(clean);
            die("invalid character in base10 input");
        }
        clean[dlen++] = in[i];
    }
    clean[dlen] = '\0';

    if (dlen == 0) {
        /* empty → single zero byte */
        free(clean);
        unsigned char *out = malloc(2);
        if (!out) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
        out[0] = 0; out[1] = '\0';
        *out_len = 1;
        return out;
    }

    uint8_t *dec = malloc(dlen);
    if (!dec) { free(clean); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < dlen; i++) dec[i] = clean[i] - '0';
    free(clean);

    size_t bcap = dlen / 2 + 2;
    unsigned char *bytes = calloc(bcap, 1);
    if (!bytes) { free(dec); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t blen = 0;

    size_t start = 0;
    while (start < dlen) {
        int all_zero = 1;
        for (size_t i = start; i < dlen; i++) if (dec[i]) { all_zero = 0; break; }
        if (all_zero) break;

        unsigned rem = 0;
        size_t new_start = start;
        int leading = 1;
        for (size_t i = start; i < dlen; i++) {
            unsigned cur = rem * 10 + dec[i];
            dec[i] = (uint8_t)(cur / 256);
            rem = cur % 256;
            if (leading && dec[i] == 0) new_start = i + 1;
            else leading = 0;
        }
        if (leading) new_start = dlen;

        if (blen >= bcap) {
            bcap *= 2;
            unsigned char *tmp = realloc(bytes, bcap);
            if (!tmp) { free(dec); free(bytes); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
            bytes = tmp;
        }
        bytes[blen++] = (unsigned char)rem;
        start = new_start;
    }

    free(dec);
    if (blen == 0) { bytes[blen++] = 0; }

    unsigned char *out = malloc(blen + 1);
    if (!out) { free(bytes); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }

    if (endian == 'b') {
        for (size_t i = 0; i < blen; i++) out[i] = bytes[blen - 1 - i];
    } else {
        memcpy(out, bytes, blen);
    }
    free(bytes);
    out[blen] = '\0';
    *out_len = blen;
    return out;
}

/* ── numeric convert (base-to-base without raw, small values) ────────────── */

static void numeric_convert(const unsigned char *in_buf, size_t in_len,
                             basespec ispec, basespec ospec)
{
    /* strip whitespace inline */
    char *clean = malloc(in_len + 1);
    if (!clean) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t ci = 0;
    for (size_t i = 0; i < in_len; i++)
        if (!isspace((unsigned char)in_buf[i])) clean[ci++] = in_buf[i];
    clean[ci] = '\0';
    if (ci == 0) { free(clean); die("empty input"); }

    char *end;
    errno = 0;
    uint64_t val = strtoull(clean, &end, ispec.base);
    if (errno == ERANGE) { free(clean); die("value out of range"); }
    if (*end != '\0')    { die_fmt("invalid input: %s", clean); }
    free(clean);

    switch (ospec.base) {
        case 2: {
            if (val == 0) { obuf_putc('0'); break; }
            char tmp[65]; int pos = 64; tmp[pos] = '\0';
            uint64_t v = val;
            while (v) { tmp[--pos] = '0' + (v & 1); v >>= 1; }
            obuf_write(tmp + pos, 64 - pos);
            break;
        }
        case 8:  { char tmp[32]; int l = snprintf(tmp, sizeof(tmp), "%" PRIo64, val); obuf_write(tmp, l); break; }
        case 10: { char tmp[32]; int l = snprintf(tmp, sizeof(tmp), "%" PRIu64, val); obuf_write(tmp, l); break; }
        case 16: { char tmp[32]; int l = snprintf(tmp, sizeof(tmp), "%" PRIx64, val); obuf_write(tmp, l); break; }
    }
    obuf_flush();
}

/* ── usage / version ─────────────────────────────────────────────────────── */

static void usage(int status)
{
    FILE *out = status ? stderr : stdout;
    fprintf(out,
"Usage: %s [OPTION]...\n"
"Convert between raw bytes and base 2, 8, 10, or 16.\n"
"Reads from standard input, writes to standard output.\n"
"\n"
"  -i BASE   input base  (default: raw bytes)\n"
"  -o BASE   output base (default: raw bytes)\n"
"  -h, --help    display this help and exit\n"
"  -V, --version output version information and exit\n"
"\n"
"BASE may be 2, 8, 10, or 16.\n"
"Append 'le' or 'be' for explicit endianness (default: be).\n"
"  Examples: 16le, 10be, 2le, 8be\n"
"\n"
"Omitting -i or -o means raw bytes.\n"
"Base 10 treats the whole input as a single arbitrarily large integer.\n"
"Bases 2, 8, 16 operate byte-by-byte; endianness controls byte order.\n"
"\n"
"Examples:\n"
"  echo -n 'hello'    | %s -o 16\n"
"  echo -n 'hello'    | %s -o 16le\n"
"  echo -n '6869'     | %s -i 16\n"
"  echo -n 'ff'       | %s -i 16 -o 10\n"
"  echo -n 'meow'     | %s -o 10le\n"
"  echo -n '255'      | %s -i 10 -o 16\n"
"\n",
    PROGRAM_NAME,
    PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME,
    PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
    exit(status);
}

static void version(void)
{
    printf("%s %s\n"
           "Copyright (C) 2026 jolly-exe\n"
           "License GPLv3+: GNU GPL version 3 or later.\n"
           "This is free software: you are free to change and redistribute it.\n"
           "There is NO WARRANTY, to the extent permitted by law.\n",
           PROGRAM_NAME, VERSION);
    exit(EXIT_SUCCESS);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    init_tables();

    basespec ispec = {0, 'b'};
    basespec ospec = {0, 'b'};
    int i_set = 0, o_set = 0;

    static struct option long_opts[] = {
        { "help",    no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:hV", long_opts, NULL)) != -1) {
        switch (c) {
            case 'i':
                if (parse_base(optarg, &ispec) != 0)
                    die_fmt("invalid base: %s", optarg);
                i_set = 1;
                break;
            case 'o':
                if (parse_base(optarg, &ospec) != 0)
                    die_fmt("invalid base: %s", optarg);
                o_set = 1;
                break;
            case 'h': usage(EXIT_SUCCESS); break;
            case 'V': version(); break;
            default:  usage(EXIT_FAILURE);
        }
    }

    if (optind < argc) {
        fprintf(stderr, "%s: extra operand '%s'\n", PROGRAM_NAME, argv[optind]);
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
        exit(EXIT_FAILURE);
    }

    /* ── dispatch ── */

    /* both base10: pure numeric */
    if (i_set && ispec.base == 10 && o_set && ospec.base == 10) {
        size_t len; unsigned char *buf = read_stdin_full(&len);
        numeric_convert(buf, len, ispec, ospec);
        free(buf);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* base10 output (raw or non-raw input) */
    if (o_set && ospec.base == 10) {
        size_t raw_len;
        unsigned char *raw;
        if (!i_set) {
            raw = read_stdin_full(&raw_len);
        } else {
            /* decode non-10 base first */
            size_t in_len; unsigned char *in_buf = read_stdin_full(&in_len);
            /* decode into raw bytes via stream_decode path but need buffer */
            /* re-use full-buffer decode */
            size_t dpb = (ispec.base == 2) ? 8 : (ispec.base == 8) ? 3 : 2;
            const uint8_t *dtab = (ispec.base == 2) ? BIN_DEC :
                                  (ispec.base == 8)  ? OCT_DEC : HEX_DEC;
            int shift = (ispec.base == 2) ? 1 : (ispec.base == 8) ? 3 : 4;
            int acc_need = (int)(dpb * shift);
            raw_len = 0;
            size_t rcap = in_len / dpb + 2;
            raw = malloc(rcap);
            if (!raw) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
            unsigned acc = 0; int acc_bits = 0;
            for (size_t i = 0; i < in_len; i++) {
                uint8_t d = dtab[(unsigned char)in_buf[i]];
                if (d == 0xff) continue;
                acc = (acc << shift) | d;
                acc_bits += shift;
                if (acc_bits == acc_need) {
                    raw[raw_len++] = (unsigned char)(acc & 0xff);
                    acc = 0; acc_bits = 0;
                }
            }
            free(in_buf);
            if (ispec.endian == 'l') {
                for (size_t i = 0; i < raw_len / 2; i++) {
                    unsigned char t = raw[i]; raw[i] = raw[raw_len-1-i]; raw[raw_len-1-i] = t;
                }
            }
        }
        encode_base10(raw, raw_len, ospec.endian);
        free(raw);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* base10 input, non-raw output */
    if (i_set && ispec.base == 10) {
        size_t in_len; unsigned char *in_buf = read_stdin_full(&in_len);
        size_t raw_len;
        unsigned char *raw = decode_base10(in_buf, in_len, ispec.endian, &raw_len);
        free(in_buf);
        if (!o_set) {
            fwrite(raw, 1, raw_len, stdout);
        } else {
            /* encode raw to base 2/8/16 */
            if (ospec.endian == 'b') {
                for (size_t i = 0; i < raw_len; i++) {
                    unsigned char b = raw[i];
                    switch (ospec.base) {
                        case 16: obuf_write((char *)&HEX_PAIR[b], 2); break;
                        case 8:  obuf_write(OCT_BYTE[b], 3); break;
                        case 2:  obuf_write(BIN_BYTE[b], 8); break;
                    }
                }
            } else {
                for (size_t i = raw_len; i > 0; i--) {
                    unsigned char b = raw[i-1];
                    switch (ospec.base) {
                        case 16: obuf_write((char *)&HEX_PAIR[b], 2); break;
                        case 8:  obuf_write(OCT_BYTE[b], 3); break;
                        case 2:  obuf_write(BIN_BYTE[b], 8); break;
                    }
                }
            }
            obuf_flush();
            if (isatty(STDOUT_FILENO)) putchar('\n');
        }
        free(raw);
        return EXIT_SUCCESS;
    }

    /* raw → base 2/8/16: fully streaming */
    if (!i_set && o_set) {
        stream_encode(ospec);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* base 2/8/16 → raw: streaming */
    if (i_set && !o_set) {
        stream_decode(ispec);
        return EXIT_SUCCESS;
    }

    /* base 2/8/16 → base 2/8/16: stream decode into encode */
    if (i_set && o_set) {
        size_t in_len; unsigned char *in_buf = read_stdin_full(&in_len);
        size_t dpb = (ispec.base == 2) ? 8 : (ispec.base == 8) ? 3 : 2;
        const uint8_t *dtab = (ispec.base == 2) ? BIN_DEC :
                              (ispec.base == 8)  ? OCT_DEC : HEX_DEC;
        int shift = (ispec.base == 2) ? 1 : (ispec.base == 8) ? 3 : 4;
        int acc_need = (int)(dpb * shift);
        unsigned acc = 0; int acc_bits = 0;
        for (size_t i = 0; i < in_len; i++) {
            uint8_t d = dtab[(unsigned char)in_buf[i]];
            if (d == 0xff) continue;
            acc = (acc << shift) | d;
            acc_bits += shift;
            if (acc_bits == acc_need) {
                unsigned char b = (unsigned char)(acc & 0xff);
                switch (ospec.base) {
                    case 16: obuf_write((char *)&HEX_PAIR[b], 2); break;
                    case 8:  obuf_write(OCT_BYTE[b], 3); break;
                    case 2:  obuf_write(BIN_BYTE[b], 8); break;
                }
                acc = 0; acc_bits = 0;
            }
        }
        free(in_buf);
        obuf_flush();
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* raw → raw: passthrough */
    {
        unsigned char ibuf[IN_BUF_SIZE];
        size_t n;
        while ((n = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0)
            fwrite(ibuf, 1, n, stdout);
    }
    return EXIT_SUCCESS;
}
