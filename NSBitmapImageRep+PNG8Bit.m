//
//  NSBitmapImageRep+PNG8Bit.m
//  Pnm8bit
//
//  Created by Gerd Knops on 2/26/13.
//  Copyright (c) 2013 BITart Consulting, Gerd Knops.
// 
// Permission to use, copy, modify, and distribute this software and its
// documentation for any purpose and without fee is hereby granted, provided
// that the above copyright notice appear in all copies and that both that
// copyright notice and this permission notice appear in supporting
// documentation.  This software is provided "as is" without express or
// implied warranty.
//
#import "NSBitmapImageRep+PNG8Bit.h"
/*:>Header
#define USE_NSLOG

#include "pngquant.h"

*/

#define	ERRLog(format,...) NSLog(@"%s:%d: ERROR: "format,__FILE__,__LINE__,##__VA_ARGS__);

@implementation NSBitmapImageRep (PNG8Bit)

//*****************************************************************************
// API
//*****************************************************************************
- (NSData *)png8data {
	
	return [self png8dataWithOptions:NULL];
}
- (NSData *)png8dataWithOptions:(PNGQuantOptions *)optionPtr {
	
	NSData	*data=[self _png8dataWithOptions:optionPtr];
	
	if(!data)
	{
		ERRLog(@"_png8dataWithOptions: failed, using AppKit!");
		
		return [self representationUsingType:NSPNGFileType properties:nil];
	}
	
	return data;
}

//:-H	Private section
//*****************************************************************************
// Callbacks
//*****************************************************************************
static void png8_error_handler(png_structp png_ptr, png_const_charp msg) {
	
	[NSException raise:@"libpng exception" format:@"error: %s",msg];
}
static void png8_user_write_data(
	png_structp png_ptr,
	png_bytep data,
	png_size_t length
)
{
	NSMutableData	*md=(NSMutableData *)png_get_io_ptr(png_ptr);
	
	[md appendBytes:(const void *)data length:(NSUInteger)length];
}
static void png8_user_flush_data(png_structp png_ptr) {
}

//*****************************************************************************
// Implementation
//*****************************************************************************
- (int)png8_getPNG24Image:(png24_image *)out {
	
	CGImageRef		image=[self CGImage];
	
	if(!image) return READ_ERROR;
	
	png_uint_32		width=(png_uint_32)CGImageGetWidth(image);
	png_uint_32 	height=(png_uint_32)CGImageGetHeight(image);
	
	rgb_pixel		*pixel_data=calloc(width*height,4);
	CGColorSpaceRef	colorspace=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef	context=CGBitmapContextCreate(
		pixel_data,
		width,height,
		8,width*4,
		colorspace,
		kCGImageAlphaPremultipliedLast
	);
	
	if(!context) return READ_ERROR;
	
	CGContextDrawImage(context,CGRectMake(0.0,0.0,width,height),image);
	CGContextRelease(context);
	CGColorSpaceRelease(colorspace);
	
	// reverse premultiplication
	for(int i=0; i<width*height; i++)
	{
		if(pixel_data[i].a)
		{
			pixel_data[i]=(rgb_pixel){
				.a=pixel_data[i].a,
				.r=pixel_data[i].r*255/pixel_data[i].a,
				.g=pixel_data[i].g*255/pixel_data[i].a,
				.b=pixel_data[i].b*255/pixel_data[i].a,
			};
		}
	}
	
	out->gamma=0.45455;
	out->width=width;
	out->height=height;
	out->rgba_data=(unsigned char *)pixel_data;
	out->row_pointers=malloc(sizeof(out->row_pointers[0])*out->height);
	
	for(int i=0; i<out->height; i++)
	{
		out->row_pointers[i]=(unsigned char *)&pixel_data[width*i];
	}
	
	return SUCCESS;
}
- (NSData *)_png8dataWithOptions:(PNGQuantOptions *)optionPtr {
	
	if(!optionPtr)
	{
		PNGQuantOptions options={
			.reqcolors = 256,
			.floyd = true, // floyd-steinberg dithering
			.min_opaque_val = 1, // whether preserve opaque colors for IE (1.0=no, does not affect alpha)
			.speed_tradeoff = 3, // 1 max quality, 10 rough & fast. 3 is optimum.
			.last_index_transparent = false, // puts transparent color at last index. This is workaround for blu-ray subtitles.
			.target_mse = 0,
			.max_mse = MAX_DIFF,
		};
		
		optionPtr=&options;
	}
	
	pngquant_image input_image={}; // initializes all fields to 0
	
	if([self png8_getPNG24Image:&input_image.rwpng_image]) return nil;
	
	png8_image output_image={};
	
	prepare_image(&input_image,optionPtr);
	
	histogram *hist=get_histogram(&input_image,optionPtr);
	
	if(input_image.noise)
	{
		free(input_image.noise);
		input_image.noise=NULL;
	}
	
	colormap *palette=pngquant_quantize(hist,optionPtr);
	pam_freeacolorhist(hist);
	
	if(palette)
	{
		pngquant_remap(palette,&input_image,&output_image,optionPtr);
		pam_freecolormap(palette);
	}
	else
	{
		if(input_image.edges)
		{
			free(input_image.edges);
			input_image.edges=NULL;
		}
		
		ERRLog(@"_png8dataWithOptions: TOO_LOW_QUALITY!");
		
		return nil;
	}
	
	png_structp png_ptr=png_create_write_struct(PNG_LIBPNG_VER_STRING,&output_image,png8_error_handler,NULL);
	png_infop info_ptr=png_create_info_struct(png_ptr);
	
	if(!info_ptr)
	{
		png_destroy_write_struct(&png_ptr,NULL);
		ERRLog(@"png_create_info_struct: out of memory!");
		
		return nil;
	}
	
	NSMutableData	*data=[[NSMutableData alloc]init];
	
	png_set_write_fn(
		png_ptr,
		data,
		png8_user_write_data,
		png8_user_flush_data
	);
	
	// Palette images generally don't gain anything from filtering
	png_set_filter(png_ptr,PNG_FILTER_TYPE_BASE,PNG_FILTER_VALUE_NONE);
	
	rwpng_set_gamma(info_ptr,png_ptr,output_image.gamma);
	
	/* set the image parameters appropriately */
	int sample_depth;
	
	if(output_image.num_palette<=2)
	{
		sample_depth=1;
	}
	else if(output_image.num_palette<=4)
	{
		sample_depth=2;
	}
	else if(output_image.num_palette<=16)
	{
		sample_depth=4;
	}
	else
	{
		sample_depth=8;
	}
	
	png_set_IHDR(png_ptr,info_ptr,output_image.width,output_image.height,
		sample_depth,PNG_COLOR_TYPE_PALETTE,
		0,PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_BASE);
		
	png_set_PLTE(png_ptr,info_ptr,&output_image.palette[0],output_image.num_palette);
	
	if(output_image.num_trans>0)
	{
		png_set_tRNS(png_ptr,info_ptr,output_image.trans,output_image.num_trans,NULL);
	}
	
	png_bytepp row_pointers=rwpng_create_row_pointers(info_ptr,png_ptr,output_image.indexed_data,output_image.height,output_image.width);
	
	rwpng_write_end(&info_ptr,&png_ptr,row_pointers);
	
	free(row_pointers);
	
	pngquant_image_free(&input_image);
	pngquant_output_image_free(&output_image);
	
	return data;
}


@end
