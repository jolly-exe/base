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

static void usage(int status);
static void version(void);

typedef struct {
    int base;      /* 2, 8, 10, 16, or 0 for raw */
    int endian;    /* 'b' for BE, 'l' for LE */
} basespec;

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

/* parse "2", "8", "10", "16", "2le", "8be", "10le", "16be", etc. */
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
    if (*rest == '\0') {
        endian = 'b';
    } else if (strcmp(rest, "be") == 0) {
        endian = 'b';
    } else if (strcmp(rest, "le") == 0) {
        endian = 'l';
    } else {
        return -1;
    }

    out->base   = base;
    out->endian = endian;
    return 0;
}

static unsigned char *read_stdin(size_t *out_len)
{
    size_t cap = 65536;
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

static char *strip_ws(const unsigned char *in, size_t in_len)
{
    char *out = malloc(in_len + 1);
    if (!out) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t ci = 0;
    for (size_t i = 0; i < in_len; i++)
        if (!isspace(in[i])) out[ci++] = in[i];
    out[ci] = '\0';
    return out;
}

static void reverse_bytes(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len / 2; i++) {
        unsigned char tmp = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = tmp;
    }
}

/* bignum decimal: digits stored little-endian (digits[0] = least significant) */
static void encode_base10(const unsigned char *buf, size_t len, int endian)
{
    if (len == 0) die("empty input");

    /* result can be at most ceil(len * 8 * log10(2)) + 1 decimal digits */
    size_t maxdigits = (size_t)(len * 2.41) + 4;
    unsigned char *digits = calloc(maxdigits, 1);
    if (!digits) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t ndigits = 1; /* start with 0 */

    /* process bytes in big-endian order regardless of input endian */
    for (size_t bi = 0; bi < len; bi++) {
        unsigned byte = (endian == 'b') ? buf[bi] : buf[len - 1 - bi];

        /* multiply existing number by 256 */
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

        /* add current byte */
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

    /* print most-significant first */
    for (size_t i = ndigits; i > 0; i--)
        putchar('0' + digits[i - 1]);

    free(digits);
}

/* decode arbitrarily large decimal string to raw bytes */
static unsigned char *decode_base10(const unsigned char *in, size_t in_len,
                                     int endian, size_t *out_len)
{
    char *clean = strip_ws(in, in_len);
    if (clean[0] == '\0') die("empty input");

    size_t dlen = strlen(clean);
    for (size_t i = 0; i < dlen; i++)
        if (!isdigit((unsigned char)clean[i])) { die_fmt("invalid input: %s", clean); }

    /* work with decimal digit array (mutable copy) */
    unsigned char *dec = malloc(dlen + 1);
    if (!dec) { free(clean); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < dlen; i++) dec[i] = clean[i] - '0';
    free(clean);

    /* result bytes stored in a growing buffer (little-endian during build) */
    size_t bcap = dlen / 2 + 2;
    unsigned char *bytes = calloc(bcap, 1);
    if (!bytes) { free(dec); perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t blen = 0;

    /* repeatedly divide dec[] by 256, collect remainders as bytes */
    size_t start = 0;
    while (start < dlen) {
        /* check all remaining digits are zero */
        int all_zero = 1;
        for (size_t i = start; i < dlen; i++) if (dec[i]) { all_zero = 0; break; }
        if (all_zero) break;

        /* divide dec[start..dlen) by 256 in place */
        unsigned rem = 0;
        size_t new_start = start;
        int leading = 1;
        for (size_t i = start; i < dlen; i++) {
            unsigned cur = rem * 10 + dec[i];
            dec[i] = cur / 256;
            rem = cur % 256;
            if (leading && dec[i] == 0) new_start = i + 1;
            else leading = 0;
        }
        if (leading) new_start = dlen;

        /* grow if needed */
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

    /* bytes[] is currently little-endian; output according to requested endian */
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

static void numeric_convert(const unsigned char *in_buf, size_t in_len,
                             basespec ispec, basespec ospec)
{
    char *clean = strip_ws(in_buf, in_len);
    if (clean[0] == '\0') die("empty input");

    char *end;
    errno = 0;
    uint64_t val = strtoull(clean, &end, ispec.base);

    if (errno == ERANGE) { free(clean); die("value out of range (max uint64)"); }
    if (*end != '\0')    { die_fmt("invalid input: %s", clean); }
    free(clean);

    switch (ospec.base) {
        case 2: {
            if (val == 0) { printf("0"); break; }
            char tmp[65];
            int pos = 64;
            tmp[pos] = '\0';
            uint64_t v = val;
            while (v) { tmp[--pos] = '0' + (v & 1); v >>= 1; }
            printf("%s", tmp + pos);
            break;
        }
        case 8:  printf("%" PRIo64, val); break;
        case 10: printf("%" PRIu64, val); break;
        case 16: printf("%" PRIx64, val); break;
    }
}

static unsigned char *decode_base(const unsigned char *in, size_t in_len,
                                   basespec spec, size_t *out_len)
{
    char *clean = malloc(in_len + 1);
    if (!clean) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }
    size_t ci = 0;

    for (size_t i = 0; i < in_len; i++) {
        if (isspace(in[i])) continue;
        clean[ci++] = in[i];
    }
    clean[ci] = '\0';

    size_t digits_per_byte = (spec.base == 2) ? 8 : (spec.base == 8) ? 3 : 2;

    if (ci % digits_per_byte != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "input length is not a multiple of %zu digits",
                 digits_per_byte);
        free(clean);
        die(msg);
    }

    *out_len = ci / digits_per_byte;
    unsigned char *out = malloc(*out_len + 1);
    if (!out) { perror(PROGRAM_NAME); exit(EXIT_FAILURE); }

    for (size_t i = 0; i < *out_len; i++) {
        char tok[9];
        memcpy(tok, clean + i * digits_per_byte, digits_per_byte);
        tok[digits_per_byte] = '\0';

        char *end;
        errno = 0;
        unsigned long val = strtoul(tok, &end, spec.base);
        if (errno || *end != '\0' || val > 255) {
            free(clean); free(out);
            die_fmt("invalid token: %s", tok);
        }
        out[i] = (unsigned char)val;
    }

    free(clean);
    out[*out_len] = '\0';

    if (spec.endian == 'l')
        reverse_bytes(out, *out_len);

    return out;
}

static void encode_base(const unsigned char *buf, size_t len, basespec spec)
{
    if (spec.endian == 'l') {
        for (size_t i = len; i > 0; i--) {
            switch (spec.base) {
                case 2:
                    for (int b = 7; b >= 0; b--)
                        putchar((buf[i-1] >> b) & 1 ? '1' : '0');
                    break;
                case 8:  printf("%03o", buf[i-1]); break;
                case 16: printf("%02x", buf[i-1]); break;
            }
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            switch (spec.base) {
                case 2:
                    for (int b = 7; b >= 0; b--)
                        putchar((buf[i] >> b) & 1 ? '1' : '0');
                    break;
                case 8:  printf("%03o", buf[i]); break;
                case 16: printf("%02x", buf[i]); break;
            }
        }
    }
}

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
"Base 10 treats the whole input as a single integer (max uint64, max 8 bytes).\n"
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

int main(int argc, char *argv[])
{
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

    size_t in_len;
    unsigned char *in_buf = read_stdin(&in_len);

    /* both bases are 10: pure numeric conversion */
    if (i_set && ispec.base == 10 && o_set && ospec.base == 10) {
        numeric_convert(in_buf, in_len, ispec, ospec);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        free(in_buf);
        return EXIT_SUCCESS;
    }

    /* non-raw non-10 input, base10 output: decode bytes then treat as int */
    if (i_set && ispec.base != 10 && o_set && ospec.base == 10) {
        size_t raw_len;
        unsigned char *raw = decode_base(in_buf, in_len, ispec, &raw_len);
        free(in_buf);
        encode_base10(raw, raw_len, ospec.endian);
        free(raw);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* base10 input, non-raw non-10 output: decode int then encode bytes */
    if (i_set && ispec.base == 10 && o_set && ospec.base != 10) {
        size_t raw_len;
        unsigned char *raw = decode_base10(in_buf, in_len, ispec.endian, &raw_len);
        free(in_buf);
        encode_base(raw, raw_len, ospec);
        free(raw);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* raw input, base10 output */
    if (!i_set && o_set && ospec.base == 10) {
        encode_base10(in_buf, in_len, ospec.endian);
        free(in_buf);
        if (isatty(STDOUT_FILENO)) putchar('\n');
        return EXIT_SUCCESS;
    }

    /* base10 input, raw output */
    if (i_set && ispec.base == 10 && !o_set) {
        size_t raw_len;
        unsigned char *raw = decode_base10(in_buf, in_len, ispec.endian, &raw_len);
        free(in_buf);
        fwrite(raw, 1, raw_len, stdout);
        free(raw);
        return EXIT_SUCCESS;
    }

    /* byte-by-byte for bases 2, 8, 16 and raw */
    unsigned char *raw;
    size_t raw_len;

    if (!i_set) {
        raw = in_buf;
        raw_len = in_len;
    } else {
        raw = decode_base(in_buf, in_len, ispec, &raw_len);
        free(in_buf);
    }

    if (!o_set) {
        fwrite(raw, 1, raw_len, stdout);
    } else {
        encode_base(raw, raw_len, ospec);
        if (isatty(STDOUT_FILENO)) putchar('\n');
    }

    free(raw);
    return EXIT_SUCCESS;
}
