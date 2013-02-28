
#ifndef PALETTE_H
#define PALETTE_H

#include "rwpng.h"
#include "pam.h"

typedef struct pngquant_options {
    double target_mse, max_mse;
    float min_opaque_val;
    unsigned int reqcolors;
    unsigned int speed_tradeoff;
    bool floyd, last_index_transparent;
    bool using_stdin, force;
    void (*log_callback)(void *context, const char *msg);
    void (*log_callback_flush)(void *context);
    void *log_callback_context;
} PNGQuantOptions;

typedef struct {
    png24_image rwpng_image;
    float *noise, *edges;
    bool modified;
} pngquant_image;

#ifdef USE_NSLOG
	#ifdef DEBUG
		#define	verbose_printf(options,format,...) NSLog(@"%s:%d: "format,__FILE__,__LINE__,##__VA_ARGS__);
	#else
		#define verbose_printf(...)
	#endif
#else
	void verbose_printf(const struct pngquant_options *context, const char *fmt, ...);
#endif

void pngquant_image_free(pngquant_image *input_image);
void pngquant_output_image_free(png8_image *output_image);
int compare_popularity(const void *ch1, const void *ch2);
void sort_palette(colormap *map, const struct pngquant_options *options);
void set_palette(png8_image *output_image, const colormap *map);
float remap_to_palette(png24_image *input_image, png8_image *output_image, colormap *const map, const float min_opaque_val);
float distance_from_closest_other_color(const colormap *map, const unsigned int i);
void remap_to_palette_floyd(png24_image *input_image, png8_image *output_image, const colormap *map, const float min_opaque_val, const float *dither_map, const int output_image_is_remapped, const float max_dither_error);
histogram *get_histogram(pngquant_image *input_image, struct pngquant_options *options);
void modify_alpha(png24_image *input_image, const float min_opaque_val);
void contrast_maps(const rgb_pixel*const apixels[], const unsigned int cols, const unsigned int rows, const double gamma, float **noiseP, float **edgesP);
void update_dither_map(const png8_image *output_image, float *edges);
void adjust_histogram_callback(hist_item *item, float diff);
colormap *find_best_palette(histogram *hist, unsigned int reqcolors, int feedback_loop_trials, const struct pngquant_options *options, double *palette_error_p);
void prepare_image(pngquant_image *input_image, struct pngquant_options *options);
colormap *pngquant_quantize(histogram *hist, const struct pngquant_options *options);
pngquant_error pngquant_remap(colormap *acolormap, pngquant_image *input_image, png8_image *output_image, const struct pngquant_options *options);

#endif

