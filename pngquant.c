/* pngquant.c - quantize the colors in an alphamap down to a specified number
**
** Copyright (C) 1989, 1991 by Jef Poskanzer.
** Copyright (C) 1997, 2000, 2002 by Greg Roelofs; based on an idea by
**                                Stefan Schneider.
** © 2009-2013 by Kornel Lesinski.
**
** Permission to use, copy, modify, and distribute this software and its
** documentation for any purpose and without fee is hereby granted, provided
** that the above copyright notice appear in all copies and that both that
** copyright notice and this permission notice appear in supporting
** documentation.  This software is provided "as is" without express or
** implied warranty.
*/

#define PNGQUANT_VERSION "1.8.3 (February 2013)"

#define PNGQUANT_USAGE "\
usage:  pngquant [options] [ncolors] [pngfile [pngfile ...]]\n\n\
options:\n\
  --force           overwrite existing output files (synonym: -f)\n\
  --nofs            disable Floyd-Steinberg dithering\n\
  --ext new.png     set custom suffix/extension for output filename\n\
  --speed N         speed/quality trade-off. 1=slow, 3=default, 10=fast & rough\n\
  --quality min-max don't save below min, use less colors below max (0-100)\n\
  --verbose         print status messages (synonym: -v)\n\
  --iebug           increase opacity to work around Internet Explorer 6 bug\n\
  --transbug        transparent color will be placed at the end of the palette\n\
\n\
Quantizes one or more 32-bit RGBA PNGs to 8-bit (or smaller) RGBA-palette\n\
PNGs using Floyd-Steinberg diffusion dithering (unless disabled).\n\
The output filename is the same as the input name except that\n\
it ends in \"-fs8.png\", \"-or8.png\" or your custom extension (unless the\n\
input is stdin, in which case the quantized image will go to stdout).\n\
The default behavior if the output file exists is to skip the conversion;\n\
use --force to overwrite.\n"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <getopt.h>

#if defined(WIN32) || defined(__WIN32__)
#  include <fcntl.h>    /* O_BINARY */
#  include <io.h>   /* setmode() */
#endif

#ifdef _OPENMP
#include <omp.h>
#else
#define omp_get_max_threads() 1
#define omp_get_thread_num() 0
#endif

#include "palette.h"
#include "rwpng.h"  /* typedefs, common macros, public prototypes */
#include "pam.h"
#include "mediancut.h"
#include "nearest.h"
#include "blur.h"
#include "viter.h"

static pngquant_error read_image(const char *filename, int using_stdin, png24_image *input_image_p);
static pngquant_error write_image(png8_image *output_image, png24_image *output_image24, const char *outname, struct pngquant_options *options);
static char *add_filename_extension(const char *filename, const char *newext);
static bool file_exists(const char *outname);

inline static void verbose_print(const struct pngquant_options *context, const char *msg)
{
    if (context->log_callback) context->log_callback(context->log_callback_context, msg);
}

static void log_callback(void *context, const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void verbose_printf_flush(struct pngquant_options *context)
{
    if (context->log_callback_flush) context->log_callback_flush(context->log_callback_context);
}

#ifdef _OPENMP
#define LOG_BUFFER_SIZE 1300
struct buffered_log {
    int buf_used;
    char buf[LOG_BUFFER_SIZE];
};

static void log_callback_buferred_flush(void *context)
{
    struct buffered_log *log = context;
    if (log->buf_used) {
        fwrite(log->buf, 1, log->buf_used, stderr);
        log->buf_used = 0;
    }
}

static void log_callback_buferred(void *context, const char *msg)
{
    struct buffered_log *log = context;
    int len = MIN(LOG_BUFFER_SIZE-1, strlen(msg));

    if (len > LOG_BUFFER_SIZE - log->buf_used - 2) log_callback_buferred_flush(log);
    memcpy(&log->buf[log->buf_used], msg, len);
    log->buf_used += len+1;
    assert(log->buf_used < LOG_BUFFER_SIZE);
    log->buf[log->buf_used-1] = '\n';
    log->buf[log->buf_used] = '\0';
}
#endif

static void print_full_version(FILE *fd)
{
    fprintf(fd, "pngquant, %s, by Greg Roelofs, Kornel Lesinski.\n"
        #ifndef NDEBUG
                    "   DEBUG (slow) version.\n"
        #endif
        #if USE_SSE
                    "   Compiled with SSE2 instructions.\n"
        #endif
        #if _OPENMP
                    "   Compiled with OpenMP (multicore support).\n"
        #endif
        , PNGQUANT_VERSION);
    rwpng_version_info(fd);
    fputs("\n", fd);
}

static void print_usage(FILE *fd)
{
    fputs(PNGQUANT_USAGE, fd);
}

#if USE_SSE
inline static bool is_sse2_available()
{
#if (defined(__x86_64__) || defined(__amd64))
    return true;
#endif
    int a,b,c,d;
        cpuid(1, a, b, c, d);
    return d & (1<<26); // edx bit 26 is set when SSE2 is present
        }
#endif

static double quality_to_mse(long quality)
{
    if (quality == 0) return MAX_DIFF;

    // curve fudged to be roughly similar to quality of libjpeg
    return 2.5/pow(210.0 + quality, 1.2) * (100.1-quality)/100.0;
}


/**
 *   N = automatic quality, uses limit unless force is set (N-N or 0-N)
 *  -N = no better than N (same as 0-N)
 * N-M = no worse than N, no better than M
 * N-  = no worse than N, perfect if possible (same as N-100)
 *
 * where N,M are numbers between 0 (lousy) and 100 (perfect)
 */
static bool parse_quality(const char *quality, struct pngquant_options *options)
{
    long limit, target;
    const char *str = quality; char *end;

    long t1 = strtol(str, &end, 10);
    if (str == end) return false;
    str = end;

    if ('\0' == end[0] && t1 < 0) { // quality="-%d"
        target = -t1;
        limit = 0;
    } else if ('\0' == end[0]) { // quality="%d"
        target = t1;
        limit = t1*9/10;
    } else if ('-' == end[0] && '\0' == end[1]) { // quality="%d-"
        target = 100;
        limit = t1;
    } else { // quality="%d-%d"
        long t2 = strtol(str, &end, 10);
        if (str == end || t2 > 0) return false;
        target = -t2;
        limit = t1;
    }

    if (target < 0 || target > 100 || limit < 0 || limit > 100) return false;

    options->max_mse = quality_to_mse(limit);
    options->target_mse = quality_to_mse(target);
    return true;
}

static const struct {const char *old; char *new;} obsolete_options[] = {
    {"-fs","--floyd"},
    {"-nofs", "--ordered"},
    {"-floyd", "--floyd"},
    {"-nofloyd", "--ordered"},
    {"-ordered", "--ordered"},
    {"-force", "--force"},
    {"-noforce", "--no-force"},
    {"-verbose", "--verbose"},
    {"-quiet", "--quiet"},
    {"-noverbose", "--quiet"},
    {"-noquiet", "--verbose"},
    {"-help", "--help"},
    {"-version", "--version"},
    {"-ext", "--ext"},
    {"-speed", "--speed"},
};

static void fix_obsolete_options(const unsigned int argc, char *argv[])
{
    for(unsigned int argn=1; argn < argc; argn++) {
        if ('-' != argv[argn][0]) continue;

        if ('-' == argv[argn][1]) break; // stop on first --option or --

        for(unsigned int i=0; i < sizeof(obsolete_options)/sizeof(obsolete_options[0]); i++) {
            if (0 == strcmp(obsolete_options[i].old, argv[argn])) {
                fprintf(stderr, "  warning: option '%s' has been replaced with '%s'.\n", obsolete_options[i].old, obsolete_options[i].new);
                argv[argn] = obsolete_options[i].new;
            }
        }
    }
}

enum {arg_floyd=1, arg_ordered, arg_ext, arg_no_force, arg_iebug, arg_transbug, arg_quality};

static const struct option long_options[] = {
    {"verbose", no_argument, NULL, 'v'},
    {"quiet", no_argument, NULL, 'q'},
    {"force", no_argument, NULL, 'f'},
    {"no-force", no_argument, NULL, arg_no_force},
    {"floyd", no_argument, NULL, arg_floyd},
    {"ordered", no_argument, NULL, arg_ordered},
    {"nofs", no_argument, NULL, arg_ordered},
    {"iebug", no_argument, NULL, arg_iebug},
    {"transbug", no_argument, NULL, arg_transbug},
    {"ext", required_argument, NULL, arg_ext},
    {"speed", required_argument, NULL, 's'},
    {"quality", required_argument, NULL, arg_quality},
    {"version", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
};

int pngquant_file(const char *filename, const char *newext, struct pngquant_options *options);

int main(int argc, char *argv[])
{
    struct pngquant_options options = {
        .reqcolors = 256,
        .floyd = true, // floyd-steinberg dithering
        .min_opaque_val = 1, // whether preserve opaque colors for IE (1.0=no, does not affect alpha)
        .speed_tradeoff = 3, // 1 max quality, 10 rough & fast. 3 is optimum.
        .last_index_transparent = false, // puts transparent color at last index. This is workaround for blu-ray subtitles.
        .target_mse = 0,
        .max_mse = MAX_DIFF,
    };
    unsigned int error_count=0, skipped_count=0, file_count=0;
    pngquant_error latest_error=SUCCESS;
    const char *newext = NULL;

    fix_obsolete_options(argc, argv);

    int opt;
    do {
        opt = getopt_long(argc, argv, "Vvqfhs:", long_options, NULL);
        switch (opt) {
            case 'v': options.log_callback = log_callback; break;
            case 'q': options.log_callback = NULL; break;
            case arg_floyd: options.floyd = true; break;
            case arg_ordered: options.floyd = false; break;
            case 'f': options.force = true; break;
            case arg_no_force: options.force = false; break;
            case arg_ext: newext = optarg; break;

            case arg_iebug:
            options.min_opaque_val = 238.0/256.0; // opacities above 238 will be rounded up to 255, because IE6 truncates <255 to 0.
                break;
            case arg_transbug:
                options.last_index_transparent = true;
                break;

            case 's':
                options.speed_tradeoff = atoi(optarg);
                if (options.speed_tradeoff < 1 || options.speed_tradeoff > 10) {
                    fputs("Speed should be between 1 (slow) and 10 (fast).\n", stderr);
                    return INVALID_ARGUMENT;
                }
                break;

            case arg_quality:
                if (!parse_quality(optarg, &options)) {
                    fputs("Quality should be in format min-max where min and max are numbers in range 0-100.\n", stderr);
                    return INVALID_ARGUMENT;
                }
                break;

            case 'h':
                print_full_version(stdout);
                print_usage(stdout);
                return SUCCESS;

            case 'V':
                puts(PNGQUANT_VERSION);
                return SUCCESS;

            case -1: break;

            default:
                return INVALID_ARGUMENT;
        }
    } while (opt != -1);

    int argn = optind;

    if (argn >= argc) {
        if (argn > 1) {
            fputs("No input files specified. See -h for help.\n", stderr);
        } else {
            print_full_version(stderr);
            print_usage(stderr);
        }
        return MISSING_ARGUMENT;
    }

    char *colors_end;
    unsigned long colors = strtoul(argv[argn], &colors_end, 10);
    if (colors_end != argv[argn] && '\0' == colors_end[0]) {
        options.reqcolors = colors;
        argn++;
    }

    if (options.reqcolors < 2 || options.reqcolors > 256) {
            fputs("Number of colors must be between 2 and 256.\n", stderr);
            return INVALID_ARGUMENT;
        }

    // new filename extension depends on options used. Typically basename-fs8.png
    if (newext == NULL) {
        newext = options.floyd ? "-ie-fs8.png" : "-ie-or8.png";
        if (options.min_opaque_val == 1.f) newext += 3; /* skip "-ie" */
    }

    if (argn == argc || (argn == argc-1 && 0==strcmp(argv[argn],"-"))) {
        options.using_stdin = true;
        argn = argc-1;
    }

#if USE_SSE
    if (!is_sse2_available()) {
        print_full_version(stderr);
        fputs("SSE2-capable CPU is required for this build.\n", stderr);
        return WRONG_ARCHITECTURE;
    }
#endif

    const int num_files = argc-argn;

#ifdef _OPENMP
    // if there's a lot of files, coarse parallelism can be used
    if (num_files > 2*omp_get_max_threads()) {
        omp_set_nested(0);
        omp_set_dynamic(1);
    } else {
        omp_set_nested(1);
    }
#endif

    #pragma omp parallel for \
        schedule(dynamic) reduction(+:skipped_count) reduction(+:error_count) reduction(+:file_count) shared(latest_error)
    for(int i=0; i < num_files; i++) {
        struct pngquant_options opts = options;
        const char *filename = opts.using_stdin ? "stdin" : argv[argn+i];

        #ifdef _OPENMP
        struct buffered_log buf = {};
        if (opts.log_callback && omp_get_num_threads() > 1 && num_files > 1) {
            verbose_printf_flush(&opts);
            opts.log_callback = log_callback_buferred;
            opts.log_callback_flush = log_callback_buferred_flush;
            opts.log_callback_context = &buf;
        }
        #endif

        pngquant_error retval = pngquant_file(filename, newext, &opts);

        verbose_printf_flush(&opts);

        if (retval) {
            #pragma omp critical
            {
                latest_error = retval;
            }
            if (retval == TOO_LOW_QUALITY) {
                skipped_count++;
            } else {
                error_count++;
            }
        }
        ++file_count;
    }

    if (error_count) {
        verbose_printf(&options, "There were errors quantizing %d file%s out of a total of %d file%s.",
                       error_count, (error_count == 1)? "" : "s", file_count, (file_count == 1)? "" : "s");
    }
    if (skipped_count) {
        verbose_printf(&options, "Skipped %d file%s out of a total of %d file%s.",
                       skipped_count, (skipped_count == 1)? "" : "s", file_count, (file_count == 1)? "" : "s");
    }
    if (!skipped_count && !error_count) {
        verbose_printf(&options, "No errors detected while quantizing %d image%s.",
                       file_count, (file_count == 1)? "" : "s");
    }

    verbose_printf_flush(&options);

    return latest_error;
}

int pngquant_file(const char *filename, const char *newext, struct pngquant_options *options)
{
    int retval = 0;

    verbose_printf(options, "%s:", filename);

    char *outname = NULL;
    if (!options->using_stdin) {
        outname = add_filename_extension(filename,newext);
        if (!options->force && file_exists(outname)) {
            fprintf(stderr, "  error:  %s exists; not overwriting\n", outname);
            retval = NOT_OVERWRITING_ERROR;
        }
    }

    pngquant_image input_image = {}; // initializes all fields to 0
    if (!retval) {
        retval = read_image(filename, options->using_stdin, &input_image.rwpng_image);
    }

    png8_image output_image = {};
    if (!retval) {
        verbose_printf(options, "  read %luKB file corrected for gamma %2.1f",
                       (input_image.rwpng_image.file_size+1023UL)/1024UL, 1.0/input_image.rwpng_image.gamma);

        prepare_image(&input_image, options);

        histogram *hist = get_histogram(&input_image, options);
        if (input_image.noise) {
            free(input_image.noise);
            input_image.noise = NULL;
    }

        colormap *palette = pngquant_quantize(hist, options);
        pam_freeacolorhist(hist);

        if (palette) {
            retval = pngquant_remap(palette, &input_image, &output_image, options);
            pam_freecolormap(palette);
        } else {
            if (input_image.edges) {
                free(input_image.edges);
                input_image.edges = NULL;
            }
            retval = TOO_LOW_QUALITY;
        }
    }

    if (!retval) {
        retval = write_image(&output_image, NULL, outname, options);
    } else if (TOO_LOW_QUALITY == retval && options->using_stdin) {
        // when outputting to stdout it'd be nasty to create 0-byte file
        // so if quality is too low, output 24-bit original
        if (!input_image.modified) {
            int write_retval = write_image(NULL, &input_image.rwpng_image, outname, options);
            if (write_retval) retval = write_retval;
        } else {
            // iebug preprocessing changes the original image
            fputs("  error:  can't write the original image when iebug option is enabled\n", stderr);
            retval = INVALID_ARGUMENT;
        }
    }

    pngquant_image_free(&input_image);
    pngquant_output_image_free(&output_image);

    return retval;
}

static bool file_exists(const char *outname)
{
    FILE *outfile = fopen(outname, "rb");
    if ((outfile ) != NULL) {
        fclose(outfile);
        return true;
    }
    return false;
}

/* build the output filename from the input name by inserting "-fs8" or
 * "-or8" before the ".png" extension (or by appending that plus ".png" if
 * there isn't any extension), then make sure it doesn't exist already */
static char *add_filename_extension(const char *filename, const char *newext)
{
    size_t x = strlen(filename);

    char* outname = malloc(x+4+strlen(newext)+1);

    strncpy(outname, filename, x);
    if (strncmp(outname+x-4, ".png", 4) == 0)
        strcpy(outname+x-4, newext);
    else
        strcpy(outname+x, newext);

    return outname;
}

static void set_binary_mode(FILE *fp)
{
#if defined(WIN32) || defined(__WIN32__)
    setmode(fp == stdout ? 1 : 0, O_BINARY);
#endif
}

static pngquant_error write_image(png8_image *output_image, png24_image *output_image24, const char *outname, struct pngquant_options *options)
{
    FILE *outfile;
    if (options->using_stdin) {
        set_binary_mode(stdout);
        outfile = stdout;

        if (output_image) {
            verbose_printf(options, "  writing %d-color image to stdout", output_image->num_palette);
        } else {
            verbose_print(options, "  writing truecolor image to stdout");
        }
    } else {

        if ((outfile = fopen(outname, "wb")) == NULL) {
            fprintf(stderr, "  error:  cannot open %s for writing\n", outname);
            return CANT_WRITE_ERROR;
        }

        const char *outfilename = strrchr(outname, '/');
        if (outfilename) outfilename++; else outfilename = outname;

        if (output_image) {
            verbose_printf(options, "  writing %d-color image as %s", output_image->num_palette, outfilename);
        } else {
            verbose_printf(options, "  writing truecolor image as %s", outfilename);
        }
    }

    pngquant_error retval;
    #pragma omp critical (libpng)
    {
        if (output_image) {
            retval = rwpng_write_image8(outfile, output_image);
        } else {
            retval = rwpng_write_image24(outfile, output_image24);
        }
    }

    if (retval) {
        fprintf(stderr, "  error: failed writing image to %s\n", outname);
    }

    if (!options->using_stdin)
        fclose(outfile);

    return retval;
}

static pngquant_error read_image(const char *filename, int using_stdin, png24_image *input_image_p)
{
    FILE *infile;

    if (using_stdin) {
        set_binary_mode(stdin);
        infile = stdin;
    } else if ((infile = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "  error: cannot open %s for reading\n", filename);
        return READ_ERROR;
    }

    pngquant_error retval;
    #pragma omp critical (libpng)
    {
            retval = rwpng_read_image24(infile, input_image_p);
    }

    if (!using_stdin)
        fclose(infile);

    if (retval) {
        fprintf(stderr, "  error: rwpng_read_image() error %d\n", retval);
        return retval;
    }

    return SUCCESS;
}

