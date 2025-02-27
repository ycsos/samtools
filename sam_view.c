/*  sam_view.c -- SAM<->BAM<->CRAM conversion.

    Copyright (C) 2009-2021 Genome Research Ltd.
    Portions copyright (C) 2009, 2011, 2012 Broad Institute.

    Author: Heng Li <lh3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notices and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <inttypes.h>
#include <getopt.h>
#include "htslib/sam.h"
#include "htslib/faidx.h"
#include "htslib/khash.h"
#include "htslib/thread_pool.h"
#include "htslib/hts_expr.h"
#include "samtools.h"
#include "sam_opts.h"
#include "bedidx.h"

KHASH_SET_INIT_STR(str)

typedef khash_t(str) *strhash_t;
KHASH_SET_INIT_STR(rg)
KHASH_SET_INIT_INT(aux_exists)

typedef khash_t(rg) *rghash_t;
typedef khash_t(aux_exists) *auxhash_t;

// This structure contains the settings for a samview run
typedef struct samview_settings {
    strhash_t rghash;
    strhash_t rnhash;
    strhash_t tvhash;
    int min_mapQ;
    int flag_on;
    int flag_off;
    int flag_alloff;
    int min_qlen;
    int remove_B;
    uint32_t subsam_seed;
    double subsam_frac;
    char* library;
    void* bed;
    size_t remove_aux_len;
    char** remove_aux;
    int multi_region;
    char* tag;
    hts_filter_t *filter;
    int remove_flag;
    int add_flag;
    auxhash_t remove_tag;
    auxhash_t keep_tag;
} samview_settings_t;


// TODO Add declarations of these to a viable htslib or samtools header
extern const char *bam_get_library(sam_hdr_t *header, const bam1_t *b);
extern int bam_remove_B(bam1_t *b);

// Copied from htslib/sam.c.
// TODO: we need a proper interface to find the length of an aux tag,
// or at the very make exportable versions of these in htslib.
static inline int aux_type2size(uint8_t type)
{
    switch (type) {
    case 'A': case 'c': case 'C':
        return 1;
    case 's': case 'S':
        return 2;
    case 'i': case 'I': case 'f':
        return 4;
    case 'd':
        return 8;
    case 'Z': case 'H': case 'B':
        return type;
    default:
        return 0;
    }
}

// Copied from htslib/sam.c.
static inline uint8_t *skip_aux(uint8_t *s, uint8_t *end)
{
    int size;
    uint32_t n;
    if (s >= end) return end;
    size = aux_type2size(*s); ++s; // skip type
    switch (size) {
    case 'Z':
    case 'H':
        while (s < end && *s) ++s;
        return s < end ? s + 1 : end;
    case 'B':
        if (end - s < 5) return NULL;
        size = aux_type2size(*s); ++s;
        n = le_to_u32(s);
        s += 4;
        if (size == 0 || end - s < size * n) return NULL;
        return s + size * n;
    case 0:
        return NULL;
    default:
        if (end - s < size) return NULL;
        return s + size;
    }
}

// Returns 0 to indicate read should be output 1 otherwise
static int process_aln(const sam_hdr_t *h, bam1_t *b, samview_settings_t* settings)
{
    if (settings->filter && sam_passes_filter(h, b, settings->filter) < 1)
        return 1;

    if (settings->remove_B) bam_remove_B(b);
    if (settings->min_qlen > 0) {
        int k, qlen = 0;
        uint32_t *cigar = bam_get_cigar(b);
        for (k = 0; k < b->core.n_cigar; ++k)
            if ((bam_cigar_type(bam_cigar_op(cigar[k]))&1) || bam_cigar_op(cigar[k]) == BAM_CHARD_CLIP)
                qlen += bam_cigar_oplen(cigar[k]);
        if (qlen < settings->min_qlen) return 1;
    }
    if (b->core.qual < settings->min_mapQ || ((b->core.flag & settings->flag_on) != settings->flag_on) || (b->core.flag & settings->flag_off))
        return 1;
    if (settings->flag_alloff && ((b->core.flag & settings->flag_alloff) == settings->flag_alloff))
        return 1;
    if (!settings->multi_region && settings->bed && (b->core.tid < 0 || !bed_overlap(settings->bed, sam_hdr_tid2name(h, b->core.tid), b->core.pos, bam_endpos(b))))
        return 1;
    if (settings->subsam_frac > 0.) {
        uint32_t k = __ac_Wang_hash(__ac_X31_hash_string(bam_get_qname(b)) ^ settings->subsam_seed);
        if ((double)(k&0xffffff) / 0x1000000 >= settings->subsam_frac) return 1;
    }
    if (settings->rghash) {
        uint8_t *s = bam_aux_get(b, "RG");
        if (s) {
            khint_t k = kh_get(str, settings->rghash, (char*)(s + 1));
            if (k == kh_end(settings->rghash)) return 1;
        }
    }
    if (settings->tag) {
        uint8_t *s = bam_aux_get(b, settings->tag);
        if (s) {
            if (settings->tvhash) {
                char t[32], *val;
                if (*s == 'i' || *s == 'I' || *s == 's' || *s == 'S' || *s == 'c' || *s == 'C') {
                    int ret = snprintf(t, 32, "%"PRId64, bam_aux2i(s));
                    if (ret > 0) val = t;
                    else return 1;
                } else if (*s == 'A') {
                    t[0] = *(s+1);
                    t[1] = 0;
                    val = t;
                } else {
                    val = (char *)(s+1);
                }
                khint_t k = kh_get(str, settings->tvhash, val);
                if (k == kh_end(settings->tvhash)) return 1;
            }
        } else {
            return 1;
        }
    }
    if (settings->rnhash) {
        const char* rn = bam_get_qname(b);
        if (!rn || kh_get(str, settings->rnhash, rn) == kh_end(settings->rnhash)) {
            return 1;
        }
    }
    if (settings->library) {
        const char *p = bam_get_library((sam_hdr_t*)h, b);
        if (!p || strcmp(p, settings->library) != 0) return 1;
    }
    if (settings->keep_tag) {
        uint8_t *s_from, *s_to, *end = b->data + b->l_data;
        auxhash_t h = settings->keep_tag;

        s_from = s_to = bam_get_aux(b);
        while (s_from < end) {
            int x = (int)s_from[0]<<8 | s_from[1];
            uint8_t *s = skip_aux(s_from+2, end);
            if (s == NULL) {
                print_error("view", "malformed aux data for record \"%s\"",
                            bam_get_qname(b));
                break;
            }

            if (kh_get(aux_exists, h, x) != kh_end(h) ) {
                if (s_to != s_from) memmove(s_to, s_from, s - s_from);
                s_to += s - s_from;
            }
            s_from = s;
        }
        b->l_data = s_to - b->data;

    } else if (settings->remove_tag) {
        uint8_t *s_from, *s_to, *end = b->data + b->l_data;
        auxhash_t h = settings->remove_tag;

        s_from = s_to = bam_get_aux(b);
        while (s_from < end) {
            int x = (int)s_from[0]<<8 | s_from[1];
            uint8_t *s = skip_aux(s_from+2, end);
            if (s == NULL) {
                print_error("view", "malformed aux data for record \"%s\"",
                            bam_get_qname(b));
                break;
            }

            if (kh_get(aux_exists, h, x) == kh_end(h) ) {
                if (s_to != s_from) memmove(s_to, s_from, s - s_from);
                s_to += s - s_from;
            }
            s_from = s;
        }
        b->l_data = s_to - b->data;
    }

    return 0;
}

static int usage(FILE *fp, int exit_status, int is_long_help);

static int populate_lookup_from_file(const char *subcmd, strhash_t lookup, char *fn)
{
    FILE *fp;
    char buf[1024];
    int ret = 0;
    fp = fopen(fn, "r");
    if (fp == NULL) {
        print_error_errno(subcmd, "failed to open \"%s\" for reading", fn);
        return -1;
    }

    while (ret != -1 && !feof(fp) && fscanf(fp, "%1023s", buf) > 0) {
        char *d = strdup(buf);
        if (d != NULL) {
            kh_put(str, lookup, d, &ret);
            if (ret == 0) free(d); /* Duplicate */
        } else {
            ret = -1;
        }
    }
    if (ferror(fp)) ret = -1;
    if (ret == -1) {
        print_error_errno(subcmd, "failed to read \"%s\"", fn);
    }
    fclose(fp);
    return (ret != -1) ? 0 : -1;
}

static int add_read_group_single(const char *subcmd, samview_settings_t *settings, char *name)
{
    char *d = strdup(name);
    int ret = 0;

    if (d == NULL) goto err;

    if (settings->rghash == NULL) {
        settings->rghash = kh_init(str);
        if (settings->rghash == NULL) goto err;
    }

    kh_put(str, settings->rghash, d, &ret);
    if (ret == -1) goto err;
    if (ret ==  0) free(d); /* Duplicate */
    return 0;

 err:
    print_error(subcmd, "Couldn't add \"%s\" to read group list: memory exhausted?", name);
    free(d);
    return -1;
}

static int add_read_names_file(const char *subcmd, samview_settings_t *settings, char *fn)
{
    if (settings->rnhash == NULL) {
        settings->rnhash = kh_init(str);
        if (settings->rnhash == NULL) {
            perror(NULL);
            return -1;
        }
    }
    return populate_lookup_from_file(subcmd, settings->rnhash, fn);
}

static int add_read_groups_file(const char *subcmd, samview_settings_t *settings, char *fn)
{
    if (settings->rghash == NULL) {
        settings->rghash = kh_init(str);
        if (settings->rghash == NULL) {
            perror(NULL);
            return -1;
        }
    }
    return populate_lookup_from_file(subcmd, settings->rghash, fn);
}

static int add_tag_value_single(const char *subcmd, samview_settings_t *settings, char *name)
{
    char *d = strdup(name);
    int ret = 0;

    if (d == NULL) goto err;

    if (settings->tvhash == NULL) {
        settings->tvhash = kh_init(str);
        if (settings->tvhash == NULL) goto err;
    }

    kh_put(str, settings->tvhash, d, &ret);
    if (ret == -1) goto err;
    if (ret ==  0) free(d); /* Duplicate */
    return 0;

 err:
    print_error(subcmd, "Couldn't add \"%s\" to tag values list: memory exhausted?", name);
    free(d);
    return -1;
}

static int add_tag_values_file(const char *subcmd, samview_settings_t *settings, char *fn)
{
    if (settings->tvhash == NULL) {
        settings->tvhash = kh_init(str);
        if (settings->tvhash == NULL) {
            perror(NULL);
            return -1;
        }
    }
    return populate_lookup_from_file(subcmd, settings->tvhash, fn);
}

static inline int check_sam_write1(samFile *fp, const sam_hdr_t *h, const bam1_t *b, const char *fname, int *retp)
{
    int r = sam_write1(fp, h, b);
    if (r >= 0) return r;

    if (fname) print_error_errno("view", "writing to \"%s\" failed", fname);
    else print_error_errno("view", "writing to standard output failed");

    *retp = EXIT_FAILURE;
    return r;
}

static inline void change_flag(bam1_t *b, samview_settings_t *settings)
{
    if (settings->add_flag)
        b->core.flag |= settings->add_flag;

    if (settings->remove_flag)
        b->core.flag &= ~settings->remove_flag;
}

int parse_aux_list(auxhash_t *h, char *optarg) {
    if (!*h)
        *h = kh_init(aux_exists);

    while (strlen(optarg) >= 2) {
        int x = optarg[0]<<8 | optarg[1];
        int ret = 0;
        kh_put(aux_exists, *h, x, &ret);
        if (ret < 0)
            return -1;

        optarg += 2;
        if (*optarg == ',') // allow white-space too for easy `cat file`?
            optarg++;
        else if (*optarg != 0)
            break;
    }

    if (strlen(optarg) != 0) {
        fprintf(stderr, "main_samview: Error parsing option, "
                "auxiliary tags should be exactly two characters long.\n");
        return -1;
    }

    return 0;
}

// Make mnemonic distinct values for longoption-only options
#define LONGOPT(c)  ((c) + 128)

int main_samview(int argc, char *argv[])
{
    int c, is_header = 0, is_header_only = 0, ret = 0, compress_level = -1, is_count = 0, has_index_file = 0, no_pg = 0;
    int64_t count = 0;
    samFile *in = 0, *out = 0, *un_out=0;
    FILE *fp_out = NULL;
    sam_hdr_t *header = NULL;
    char out_mode[6] = {0}, out_un_mode[6] = {0}, *out_format = "";
    char *fn_in = 0, *fn_idx_in = 0, *fn_out = 0, *fn_fai = 0, *q, *fn_un_out = 0;
    char *fn_out_idx = NULL, *fn_un_out_idx = NULL, *arg_list = NULL;
    sam_global_args ga = SAM_GLOBAL_ARGS_INIT;
    htsThreadPool p = {NULL, 0};
    int filter_state = ALL, filter_op = 0;
    int result;

    samview_settings_t settings = {
        .rghash = NULL,
        .tvhash = NULL,
        .min_mapQ = 0,
        .flag_on = 0,
        .flag_off = 0,
        .flag_alloff = 0,
        .min_qlen = 0,
        .remove_B = 0,
        .subsam_seed = 0,
        .subsam_frac = -1.,
        .library = NULL,
        .bed = NULL,
        .multi_region = 0,
        .tag = NULL,
        .filter = NULL,
        .remove_flag = 0,
        .add_flag = 0,
        .keep_tag = NULL,
        .remove_tag = NULL,
    };

    static const struct option lopts[] = {
        SAM_OPT_GLOBAL_OPTIONS('-', 0, 'O', 0, 'T', '@'),
        {"add-flags", required_argument, NULL, LONGOPT('a')},
        {"bam", no_argument, NULL, 'b'},
        {"count", no_argument, NULL, 'c'},
        {"cram", no_argument, NULL, 'C'},
        {"customised-index", no_argument, NULL, 'X'},
        {"customized-index", no_argument, NULL, 'X'},
        {"excl-flags", required_argument, NULL, 'F'},
        {"exclude-flags", required_argument, NULL, 'F'},
        {"expr", required_argument, NULL, 'e'},
        {"expression", required_argument, NULL, 'e'},
        {"fai-reference", required_argument, NULL, 't'},
        {"fast", no_argument, NULL, '1'},
        {"header-only", no_argument, NULL, 'H'},
        {"help", no_argument, NULL, LONGOPT('?')},
        {"keep-tag", required_argument, NULL, LONGOPT('x') },
        {"library", required_argument, NULL, 'l'},
        {"min-mapq", required_argument, NULL, 'q'},
        {"min-MQ", required_argument, NULL, 'q'},
        {"min-mq", required_argument, NULL, 'q'},
        {"min-qlen", required_argument, NULL, 'm'},
        {"no-header", no_argument, NULL, LONGOPT('H')},
        {"no-PG", no_argument, NULL, LONGOPT('P')},
        {"output", required_argument, NULL, 'o'},
        {"output-unselected", required_argument, NULL, 'U'},
        {"QNAME-file", required_argument, NULL, 'N'},
        {"qname-file", required_argument, NULL, 'N'},
        {"read-group", required_argument, NULL, 'r'},
        {"read-group-file", required_argument, NULL, 'R'},
        {"readgroup", required_argument, NULL, 'r'},
        {"readgroup-file", required_argument, NULL, 'R'},
        {"region-file", required_argument, NULL, LONGOPT('L')},
        {"regions-file", required_argument, NULL, LONGOPT('L')},
        {"remove-B", no_argument, NULL, 'B'},
        {"remove-flags", required_argument, NULL, LONGOPT('r')},
        {"remove-tag", required_argument, NULL, 'x'},
        {"require-flags", required_argument, NULL, 'f'},
        {"subsample", required_argument, NULL, LONGOPT('s')},
        {"subsample-seed", required_argument, NULL, LONGOPT('S')},
        {"tag", required_argument, NULL, 'd'},
        {"tag-file", required_argument, NULL, 'D'},
        {"target-file", required_argument, NULL, 'L'},
        {"targets-file", required_argument, NULL, 'L'},
        {"uncompressed", no_argument, NULL, 'u'},
        {"unoutput", required_argument, NULL, 'U'},
        {"use-index", no_argument, NULL, 'M'},
        {"with-header", no_argument, NULL, 'h'},
    };

    /* parse command-line options */
    strcpy(out_mode, "w");
    strcpy(out_un_mode, "w");
    if (argc == 1 && isatty(STDIN_FILENO))
        return usage(stdout, EXIT_SUCCESS, 0);

    // Suppress complaints about '?' being an unrecognised option.  Without
    // this we have to put '?' in the options list, which makes it hard to
    // tell a bad long option from the use of '-?' (both return '?' and
    // set optopt to '\0').
    opterr = 0;

    while ((c = getopt_long(argc, argv,
                            "SbBcCt:h1Ho:O:q:f:F:G:ul:r:T:R:N:d:D:L:s:@:m:x:U:MXe:",
                            lopts, NULL)) >= 0) {
        switch (c) {
        case 's':
            settings.subsam_seed = strtol(optarg, &q, 10);
            if (q && *q == '.') {
                settings.subsam_frac = strtod(q, &q);
                if (*q) ret = 1;
            } else {
                ret = 1;
            }

            if (ret == 1) {
                print_error("view", "Incorrect sampling argument \"%s\"", optarg);
                goto view_end;
            }
            break;
        case LONGOPT('s'):
            settings.subsam_frac = strtod(optarg, &q);
            if (*q || settings.subsam_frac < 0.0 || settings.subsam_frac > 1.0) {
                print_error("view", "Incorrect sampling argument \"%s\"", optarg);
                goto view_end;
            }
            break;
        case LONGOPT('S'): settings.subsam_seed = atoi(optarg); break;
        case 'm': settings.min_qlen = atoi(optarg); break;
        case 'c': is_count = 1; break;
        case 'S': break;
        case 'b': out_format = "b"; break;
        case 'C': out_format = "c"; break;
        case 't': fn_fai = strdup(optarg); break;
        case 'h': is_header = 1; break;
        case 'H': is_header_only = 1; break;
        case LONGOPT('H'): is_header = is_header_only = 0; break;
        case 'o': fn_out = strdup(optarg); break;
        case 'U': fn_un_out = strdup(optarg); break;
        case 'X': has_index_file = 1; break;
        case 'f': settings.flag_on |= bam_str2flag(optarg); break;
        case 'F': settings.flag_off |= bam_str2flag(optarg); break;
        case 'G': settings.flag_alloff |= bam_str2flag(optarg); break;
        case 'q': settings.min_mapQ = atoi(optarg); break;
        case 'u': compress_level = 0; break;
        case '1': compress_level = 1; break;
        case 'l': settings.library = strdup(optarg); break;
        case LONGOPT('L'):
            settings.multi_region = 1;
            // fall through
        case 'L':
            if ((settings.bed = bed_read(optarg)) == NULL) {
                print_error_errno("view", "Could not read file \"%s\"", optarg);
                ret = 1;
                goto view_end;
            }
            break;
        case 'r':
            if (add_read_group_single("view", &settings, optarg) != 0) {
                ret = 1;
                goto view_end;
            }
            break;
        case 'R':
            if (add_read_groups_file("view", &settings, optarg) != 0) {
                ret = 1;
                goto view_end;
            }
            break;
        case 'N':
            if (add_read_names_file("view", &settings, optarg) != 0) {
                ret = 1;
                goto view_end;
            }
            break;
        case 'd':
            if (strlen(optarg) < 2 || (strlen(optarg) > 2 && optarg[2] != ':')) {
                print_error_errno("view", "Invalid \"tag:value\" option: \"%s\"", optarg);
                ret = 1;
                goto view_end;
            }

            if (settings.tag) {
                if (settings.tag[0] != optarg[0] || settings.tag[1] != optarg[1]) {
                    print_error("view", "Different tag \"%s\" was specified before: \"%s\"", settings.tag, optarg);
                    ret = 1;
                    goto view_end;
                }
            } else {
                if (!(settings.tag = calloc(3, 1))) {
                    print_error("view", "Could not allocate memory for tag: \"%s\"", optarg);
                    ret = 1;
                    goto view_end;
                }
                memcpy(settings.tag, optarg, 2);
            }

            if (strlen(optarg) > 3 && add_tag_value_single("view", &settings, optarg+3) != 0) {
                print_error("view", "Could not add tag:value \"%s\"", optarg);
                ret = 1;
                goto view_end;
            }
            break;
        case 'D':
            // Allow ";" as delimiter besides ":" to support MinGW CLI POSIX
            // path translation as described at:
            // http://www.mingw.org/wiki/Posix_path_conversion
            if (strlen(optarg) < 4 || (optarg[2] != ':' && optarg[2] != ';')) {
                print_error_errno("view", "Invalid \"tag:file\" option: \"%s\"", optarg);
                ret = 1;
                goto view_end;
            }

            if (settings.tag) {
                if (settings.tag[0] != optarg[0] || settings.tag[1] != optarg[1]) {
                    print_error("view", "Different tag \"%s\" was specified before: \"%s\"", settings.tag, optarg);
                    ret = 1;
                    goto view_end;
                }
            } else {
                if (!(settings.tag = calloc(3, 1))) {
                    print_error("view", "Could not allocate memory for tag: \"%s\"", optarg);
                    ret = 1;
                    goto view_end;
                }
                memcpy(settings.tag, optarg, 2);
            }

            if (add_tag_values_file("view", &settings, optarg+3) != 0) {
                ret = 1;
                goto view_end;
            }
            break;
                /* REMOVED as htslib doesn't support this
        //case 'x': out_format = "x"; break;
        //case 'X': out_format = "X"; break;
                 */
        case LONGOPT('?'):
            return usage(stdout, EXIT_SUCCESS, 1);
        case '?':
            if (optopt == '?') {  // '-?' appeared on command line
                return usage(stdout, EXIT_SUCCESS, 1);
            } else {
                if (optopt) { // Bad short option
                    print_error("view", "invalid option -- '%c'", optopt);
                } else { // Bad long option
                    // Do our best.  There is no good solution to finding
                    // out what the bad option was.
                    // See, e.g. https://stackoverflow.com/questions/2723888/where-does-getopt-long-store-an-unrecognized-option
                    if (optind > 0 && strncmp(argv[optind - 1], "--", 2) == 0) {
                        print_error("view", "unrecognised option '%s'",
                                    argv[optind - 1]);
                    }
                }
                return usage(stderr, EXIT_FAILURE, 0);
            }
        case 'B': settings.remove_B = 1; break;

        case 'M': settings.multi_region = 1; break;
        case LONGOPT('P'): no_pg = 1; break;
        case 'e':
            if (!(settings.filter = hts_filter_init(optarg))) {
                print_error("main_samview", "Couldn't initialise filter");
                return 1;
            }
            break;
        case LONGOPT('r'): settings.remove_flag |= bam_str2flag(optarg); break;
        case LONGOPT('a'): settings.add_flag |= bam_str2flag(optarg); break;

        case 'x':
            if (*optarg == '^') {
                if (parse_aux_list(&settings.keep_tag, optarg+1))
                    return usage(stderr, EXIT_FAILURE, 0);
            } else {
                if (parse_aux_list(&settings.remove_tag, optarg))
                    return usage(stderr, EXIT_FAILURE, 0);
            }
            break;

        case LONGOPT('x'):
            if (parse_aux_list(&settings.keep_tag, optarg))
                return usage(stderr, EXIT_FAILURE, 0);
            break;

        default:
            if (parse_sam_global_opt(c, optarg, lopts, &ga) != 0)
                return usage(stderr, EXIT_FAILURE, 0);
            break;
        }
    }
    if (fn_fai == 0 && ga.reference) fn_fai = fai_path(ga.reference);
    if (compress_level >= 0 && !*out_format) out_format = "b";
    if (is_header_only) is_header = 1;
    // File format auto-detection first
    if (fn_out)    sam_open_mode(out_mode+1,    fn_out,    NULL);
    if (fn_un_out) sam_open_mode(out_un_mode+1, fn_un_out, NULL);
    // Overridden by manual -b, -C
    if (*out_format)
        out_mode[1] = out_un_mode[1] = *out_format;
    // out_(un_)mode now 1, 2 or 3 bytes long, followed by nul.
    if (compress_level >= 0) {
        char tmp[2];
        tmp[0] = compress_level + '0'; tmp[1] = '\0';
        strcat(out_mode, tmp);
        strcat(out_un_mode, tmp);
    }
    if (argc == optind && isatty(STDIN_FILENO)) {
        print_error("view", "No input provided or missing option argument.");
        return usage(stderr, EXIT_FAILURE, 0); // potential memory leak...
    }
    if (settings.subsam_seed != 0) {
        // Convert likely user input 1,2,... to pseudo-random
        // values with more entropy and more bits set
        srand(settings.subsam_seed);
        settings.subsam_seed = rand();
    }

    fn_in = (optind < argc)? argv[optind] : "-";
    if ((in = sam_open_format(fn_in, "r", &ga.in)) == 0) {
        print_error_errno("view", "failed to open \"%s\" for reading", fn_in);
        ret = 1;
        goto view_end;
    }

    if (fn_fai) {
        if (hts_set_fai_filename(in, fn_fai) != 0) {
            fprintf(stderr, "[main_samview] failed to use reference \"%s\".\n", fn_fai);
            ret = 1;
            goto view_end;
        }
    }
    if ((header = sam_hdr_read(in)) == 0) {
        fprintf(stderr, "[main_samview] fail to read the header from \"%s\".\n", fn_in);
        ret = 1;
        goto view_end;
    }
    if (settings.rghash) {
        sam_hdr_remove_lines(header, "RG", "ID", settings.rghash);
    }
    if (!is_count) {
        if ((out = sam_open_format(fn_out? fn_out : "-", out_mode, &ga.out)) == 0) {
            print_error_errno("view", "failed to open \"%s\" for writing", fn_out? fn_out : "standard output");
            ret = 1;
            goto view_end;
        }
        if (fn_fai) {
            if (hts_set_fai_filename(out, fn_fai) != 0) {
                fprintf(stderr, "[main_samview] failed to use reference \"%s\".\n", fn_fai);
                ret = 1;
                goto view_end;
            }
        }

        if (!no_pg) {
            if (!(arg_list = stringify_argv(argc+1, argv-1))) {
                print_error("view", "failed to create arg_list");
                ret = 1;
                goto view_end;
            }
            if (sam_hdr_add_pg(header, "samtools",
                                         "VN", samtools_version(),
                                         arg_list ? "CL": NULL,
                                         arg_list ? arg_list : NULL,
                                         NULL)) {
                print_error("view", "failed to add PG line to the header");
                ret = 1;
                goto view_end;
            }
        }

        if (*out_format || ga.write_index || is_header ||
            out_mode[1] == 'b' || out_mode[1] == 'c' ||
            (ga.out.format != sam && ga.out.format != unknown_format))  {
            if (sam_hdr_write(out, header) != 0) {
                fprintf(stderr, "[main_samview] failed to write the SAM header\n");
                ret = 1;
                goto view_end;
            }
        }
        if (ga.write_index) {
            if (!(fn_out_idx = auto_index(out, fn_out, header))) {
                ret = 1;
                goto view_end;
            }
        }

        if (fn_un_out) {
            if ((un_out = sam_open_format(fn_un_out, out_un_mode, &ga.out)) == 0) {
                print_error_errno("view", "failed to open \"%s\" for writing", fn_un_out);
                ret = 1;
                goto view_end;
            }
            if (fn_fai) {
                if (hts_set_fai_filename(un_out, fn_fai) != 0) {
                    fprintf(stderr, "[main_samview] failed to use reference \"%s\".\n", fn_fai);
                    ret = 1;
                    goto view_end;
                }
            }
            if (*out_format || is_header ||
                out_un_mode[1] == 'b' || out_un_mode[1] == 'c' ||
                (ga.out.format != sam && ga.out.format != unknown_format))  {
                if (sam_hdr_write(un_out, header) != 0) {
                    fprintf(stderr, "[main_samview] failed to write the SAM header\n");
                    ret = 1;
                    goto view_end;
                }
            }
            if (ga.write_index) {
                if (!(fn_un_out_idx = auto_index(un_out, fn_un_out, header))) {
                    ret = 1;
                    goto view_end;
                }
            }
        }
    }
    else {
        if (fn_out) {
            fp_out = fopen(fn_out, "w");
            if (fp_out == NULL) {
                print_error_errno("view", "can't create \"%s\"", fn_out);
                ret = EXIT_FAILURE;
                goto view_end;
            }
        }
    }

    if (ga.nthreads > 1) {
        if (!(p.pool = hts_tpool_init(ga.nthreads))) {
            fprintf(stderr, "Error creating thread pool\n");
            ret = 1;
            goto view_end;
        }
        hts_set_opt(in,  HTS_OPT_THREAD_POOL, &p);
        if (out) hts_set_opt(out, HTS_OPT_THREAD_POOL, &p);
    }
    if (is_header_only) goto view_end; // no need to print alignments

    if (has_index_file) {
        fn_idx_in = (optind+1 < argc)? argv[optind+1] : 0;
        if (fn_idx_in == 0) {
            fprintf(stderr, "[main_samview] incorrect number of arguments for -X option. Aborting.\n");
            return 1;
        }
    }

    if (settings.multi_region) {
        if (!has_index_file && optind < argc - 1) { //regions have been specified in the command line
            settings.bed = bed_hash_regions(settings.bed, argv, optind+1, argc, &filter_op); //insert(1) or filter out(0) the regions from the command line in the same hash table as the bed file
            if (!filter_op)
                filter_state = FILTERED;
        } else if (has_index_file && optind < argc - 2) {
            settings.bed = bed_hash_regions(settings.bed, argv, optind+2, argc, &filter_op); //insert(1) or filter out(0) the regions from the command line in the same hash table as the bed file
            if (!filter_op)
                filter_state = FILTERED;
        } else {
            bed_unify(settings.bed);
        }

        bam1_t *b = bam_init1();
        if (settings.bed == NULL) { // index is unavailable or no regions have been specified
            fprintf(stderr, "[main_samview] no regions or BED file have been provided. Aborting.\n");
        } else {
            hts_idx_t *idx = NULL;
            // If index filename has not been specfied, look in BAM folder
            if (fn_idx_in != 0) {
                idx = sam_index_load2(in, fn_in, fn_idx_in); // load index
            } else {
                idx = sam_index_load(in, fn_in);
            }
            if (idx != NULL) {

                int regcount = 0;

                hts_reglist_t *reglist = bed_reglist(settings.bed, filter_state, &regcount);
                if(reglist) {
                    hts_itr_multi_t *iter = sam_itr_regions(idx, header, reglist, regcount);
                    if (iter) {
                        // fetch alignments
                        while ((result = sam_itr_multi_next(in, iter, b)) >= 0) {
                            if (!process_aln(header, b, &settings)) {
                                if (!is_count) {
                                    change_flag(b, &settings);
                                    if (check_sam_write1(out, header, b, fn_out, &ret) < 0) break;
                                }
                                count++;
                            } else {
                                if (un_out) { if (check_sam_write1(un_out, header, b, fn_un_out, &ret) < 0) break; }
                            }
                        }
                        if (result < -1) {
                            fprintf(stderr, "[main_samview] retrieval of region %d failed due to truncated file or corrupt BAM index file\n", iter->curr_tid);
                            ret = 1;
                        }

                        hts_itr_multi_destroy(iter);
                    } else {
                        fprintf(stderr, "[main_samview] iterator could not be created. Aborting.\n");
                    }
                } else {
                    fprintf(stderr, "[main_samview] region list is empty or could not be created. Aborting.\n");
                }
                hts_idx_destroy(idx); // destroy the BAM index
            } else {
                fprintf(stderr, "[main_samview] random alignment retrieval only works for indexed BAM or CRAM files.\n");
            }
        }
        bam_destroy1(b);
    } else {
        if ((has_index_file && optind >= argc - 2) || (!has_index_file && optind >= argc - 1)) { // convert/print the entire file
            bam1_t *b = bam_init1();
            int r;
            errno = 0;
            while ((r = sam_read1(in, header, b)) >= 0) { // read one alignment from `in'
                if (!process_aln(header, b, &settings)) {
                    if (!is_count) {
                        change_flag(b, &settings);
                        if (check_sam_write1(out, header, b, fn_out, &ret) < 0) break;
                    }
                    count++;
                } else {
                    if (un_out) { if (check_sam_write1(un_out, header, b, fn_un_out, &ret) < 0) break; }
                }
            }
            if (r < -1) {
                print_error_errno("view", "error reading file \"%s\"", fn_in);
                ret = 1;
            }
            bam_destroy1(b);
        } else { // retrieve alignments in specified regions
            int i;
            bam1_t *b;
            hts_idx_t *idx = NULL;
            // If index filename has not been specfied, look in BAM folder
            if (fn_idx_in != NULL) {
                idx = sam_index_load2(in, fn_in, fn_idx_in); // load index
            } else {
                idx = sam_index_load(in, fn_in);
            }
            if (idx == 0) { // index is unavailable
                fprintf(stderr, "[main_samview] random alignment retrieval only works for indexed BAM or CRAM files.\n");
                ret = 1;
                goto view_end;
            }
            b = bam_init1();

            for (i = (has_index_file)? optind+2 : optind+1; i < argc; ++i) {
                int result;
                hts_itr_t *iter = sam_itr_querys(idx, header, argv[i]); // parse a region in the format like `chr2:100-200'
                if (iter == NULL) { // region invalid or reference name not found
                    fprintf(stderr, "[main_samview] region \"%s\" specifies an invalid region or unknown reference. Continue anyway.\n", argv[i]);
                    continue;
                }
                // fetch alignments
                while ((result = sam_itr_next(in, iter, b)) >= 0) {
                    if (!process_aln(header, b, &settings)) {
                        if (!is_count) {
                            change_flag(b, &settings);
                            if (check_sam_write1(out, header, b, fn_out, &ret) < 0) break;
                        }
                        count++;
                    } else {
                        if (un_out) { if (check_sam_write1(un_out, header, b, fn_un_out, &ret) < 0) break; }
                    }
                }
                hts_itr_destroy(iter);
                if (result < -1) {
                    fprintf(stderr, "[main_samview] retrieval of region \"%s\" failed due to truncated file or corrupt BAM index file\n", argv[i]);
                    ret = 1;
                    break;
                }
            }
            bam_destroy1(b);
            hts_idx_destroy(idx); // destroy the BAM index
        }
    }

    if (ga.write_index) {
        if (sam_idx_save(out) < 0) {
            print_error_errno("view", "writing index failed");
            ret = 1;
        }
        if (un_out && sam_idx_save(un_out) < 0) {
            print_error_errno("view", "writing index failed");
            ret = 1;
        }
    }

view_end:
    if (is_count && ret == 0) {
        if (fprintf(fn_out? fp_out : stdout, "%" PRId64 "\n", count) < 0) {
            if (fn_out) print_error_errno("view", "writing to \"%s\" failed", fn_out);
            else print_error_errno("view", "writing to standard output failed");
            ret = EXIT_FAILURE;
        }
    }

    // close files, free and return
    if (in) check_sam_close("view", in, fn_in, "standard input", &ret);
    if (out) check_sam_close("view", out, fn_out, "standard output", &ret);
    if (un_out) check_sam_close("view", un_out, fn_un_out, "file", &ret);
    if (fp_out) fclose(fp_out);

    free(fn_fai); free(fn_out); free(settings.library);  free(fn_un_out);
    sam_global_args_free(&ga);
    if ( header ) sam_hdr_destroy(header);
    if (settings.bed) bed_destroy(settings.bed);
    if (settings.rghash) {
        khint_t k;
        for (k = 0; k < kh_end(settings.rghash); ++k)
            if (kh_exist(settings.rghash, k)) free((char*)kh_key(settings.rghash, k));
        kh_destroy(str, settings.rghash);
    }
    if (settings.rnhash) {
        khint_t k;
        for (k = 0; k < kh_end(settings.rnhash); ++k)
            if (kh_exist(settings.rnhash, k)) free((char*)kh_key(settings.rnhash, k));
        kh_destroy(str, settings.rnhash);
    }
    if (settings.tvhash) {
        khint_t k;
        for (k = 0; k < kh_end(settings.tvhash); ++k)
            if (kh_exist(settings.tvhash, k)) free((char*)kh_key(settings.tvhash, k));
        kh_destroy(str, settings.tvhash);
    }

    if (settings.remove_aux_len) {
        free(settings.remove_aux);
    }
    if (settings.tag) {
        free(settings.tag);
    }
    if (settings.filter)
        hts_filter_free(settings.filter);

    if (p.pool)
        hts_tpool_destroy(p.pool);

    if (fn_out_idx)
        free(fn_out_idx);
    if (fn_un_out_idx)
        free(fn_un_out_idx);
    free(arg_list);

    if (settings.keep_tag)
        kh_destroy(aux_exists, settings.keep_tag);
    if (settings.remove_tag)
        kh_destroy(aux_exists, settings.remove_tag);

    return ret;
}

static int usage(FILE *fp, int exit_status, int is_long_help)
{
    fprintf(fp,
"\n"
"Usage: samtools view [options] <in.bam>|<in.sam>|<in.cram> [region ...]\n"
"\n"

"Output options:\n"
"  -b, --bam                  Output BAM\n"
"  -C, --cram                 Output CRAM (requires -T)\n"
"  -1, --fast                 Use fast BAM compression (implies --bam)\n"
"  -u, --uncompressed         Uncompressed BAM output (implies --bam)\n"
"  -h, --with-header          Include header in SAM output\n"
"  -H, --header-only          Print SAM header only (no alignments)\n"
"      --no-header            Print SAM alignment records only [default]\n"
"  -c, --count                Print only the count of matching records\n"
"  -o, --output FILE          Write output to FILE [standard output]\n"
"  -U, --unoutput FILE, --output-unselected FILE\n"
"                             Output reads not selected by filters to FILE\n"
"Input options:\n"
"  -t, --fai-reference FILE   FILE listing reference names and lengths\n"
"  -M, --use-index            Use index and multi-region iterator for regions\n"
"      --region[s]-file FILE  Use index to include only reads overlapping FILE\n"
"  -X, --customized-index     Expect extra index file argument after <in.bam>\n"
"\n"
"Filtering options (Only include in output reads that...):\n"
"  -L, --target[s]-file FILE  ...overlap (BED) regions in FILE\n"
"  -r, --read-group STR       ...are in read group STR\n"
"  -R, --read-group-file FILE ...are in a read group listed in FILE\n"
"  -N, --qname-file FILE      ...whose read name is listed in FILE\n"
"  -d, --tag STR1[:STR2]      ...have a tag STR1 (with associated value STR2)\n"
"  -D, --tag-file STR:FILE    ...have a tag STR whose value is listed in FILE\n"
"  -q, --min-MQ INT           ...have mapping quality >= INT\n"
"  -l, --library STR          ...are in library STR\n"
"  -m, --min-qlen INT         ...cover >= INT query bases (as measured via CIGAR)\n"
"  -e, --expr STR             ...match the filter expression STR\n"
"  -f, --require-flags FLAG   ...have all of the FLAGs present\n"             //   F&x == x
"  -F, --excl[ude]-flags FLAG ...have none of the FLAGs present\n"            //   F&x == 0
"  -G FLAG                    EXCLUDE reads with all of the FLAGs present\n"  // !(F&x == x)  TODO long option
"      --subsample FLOAT      Keep only FLOAT fraction of templates/read pairs\n"
"      --subsample-seed INT   Influence WHICH reads are kept in subsampling [0]\n"
"  -s INT.FRAC                Same as --subsample 0.FRAC --subsample-seed INT\n"
"\n"
"Processing options:\n"
"      --add-flags FLAG       Add FLAGs to reads\n"
"      --remove-flags FLAG    Remove FLAGs from reads\n"
"  -x, --remove-tag STR\n"
"               Comma-separated read tags to strip (repeatable) [null]\n"
"      --keep-tag STR\n"
"               Comma-separated read tags to preserve (repeatable) [null].\n"
"               Equivalent to \"-x ^STR\"\n"
"  -B, --remove-B             Collapse the backward CIGAR operation\n"
"\n"
"General options:\n"
"  -?, --help   Print long help, including note about region specification\n"
"  -S           Ignored (input format is auto-detected)\n"
"      --no-PG  Do not add a PG line\n");

    sam_global_opt_help(fp, "-.O.T@..");
    fprintf(fp, "\n");

    if (is_long_help)
        fprintf(fp,
"Notes:\n"
"\n"
"1. This command now auto-detects the input format (BAM/CRAM/SAM).\n"
"   Further control over the CRAM format can be specified by using the\n"
"   --output-fmt-option, e.g. to specify the number of sequences per slice\n"
"   and to use avoid reference based compression:\n"
"\n"
"\tsamtools view -C --output-fmt-option seqs_per_slice=5000 \\\n"
"\t   --output-fmt-option no_ref -o out.cram in.bam\n"
"\n"
"   Options can also be specified as a comma separated list within the\n"
"   --output-fmt value too.  For example this is equivalent to the above\n"
"\n"
"\tsamtools view --output-fmt cram,seqs_per_slice=5000,no_ref \\\n"
"\t   -o out.cram in.bam\n"
"\n"
"2. The file supplied with `-t' is SPACE/TAB delimited with the first\n"
"   two fields of each line consisting of the reference name and the\n"
"   corresponding sequence length. The `.fai' file generated by \n"
"   `samtools faidx' is suitable for use as this file. This may be an\n"
"   empty file if reads are unaligned.\n"
"\n"
"3. SAM->BAM conversion:  samtools view -bT ref.fa in.sam.gz\n"
"\n"
"4. BAM->SAM conversion:  samtools view -h in.bam\n"
"\n"
"5. A region should be presented in one of the following formats:\n"
"   `chr1', `chr2:1,000' and `chr3:1000-2,000'. When a region is\n"
"   specified, the input alignment file must be a sorted and indexed\n"
"   alignment (BAM/CRAM) file.\n"
"\n"
"6. Option `-u' is preferred over `-b' when the output is piped to\n"
"   another samtools command.\n"
"\n"
"7. Option `-M`/`--use-index` causes overlaps with `-L` BED file regions and\n"
"   command-line region arguments to be computed using the multi-region iterator\n"
"   and an index. This increases speed, omits duplicates, and outputs the reads\n"
"   as they are ordered in the input SAM/BAM/CRAM file.\n"
"\n"
"8. Options `-L`/`--target[s]-file` and `--region[s]-file` may not be used\n"
"   together. `--region[s]-file FILE` is simply equivalent to `-M -L FILE`,\n"
"   so using both causes one of the specified BED files to be ignored.\n"
"\n");

    return exit_status;
}
