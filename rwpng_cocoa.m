//
//  rwpng_cocoa.m
//  pngquant
//

#include "rwpng.h"

#ifdef USE_COCOA

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include "pam.h"

int rwpng_read_image24_cocoa(FILE *fp, png24_image *out)
{
    rgb_pixel *pixel_data;
    int width, height;
    @autoreleasepool {
        NSFileHandle *fh = [[NSFileHandle alloc] initWithFileDescriptor:fileno(fp)];
        NSData *data = [fh readDataToEndOfFile];
        out->file_size = [data length];
        
        #ifdef SUPPORT_PDF
        NSImage			*src=[[NSImage alloc]initWithData:data];
        NSData			*imgData=[src TIFFRepresentation];
        CGImageRef image = [[NSBitmapImageRep imageRepWithData:imgData] CGImage];
        [src release];
        #else
        CGImageRef image = [[NSBitmapImageRep imageRepWithData:data] CGImage];
        #endif
        
        [fh release];

        if (!image) return READ_ERROR;

        width = CGImageGetWidth(image);
        height = CGImageGetHeight(image);

        pixel_data = calloc(width*height,4);

        CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

        CGContextRef context = CGBitmapContextCreate(pixel_data,
                                                     width, height,
                                                     8, width*4,
                                                     colorspace,
                                                     kCGImageAlphaPremultipliedLast);

        if (!context) return READ_ERROR;

        CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, height), image);
        CGContextRelease(context);
        CGColorSpaceRelease(colorspace);
    }
    // reverse premultiplication

    for(int i=0; i < width*height; i++) {
        if (pixel_data[i].a) {
            pixel_data[i] = (rgb_pixel){
                .a = pixel_data[i].a,
                .r = pixel_data[i].r*255/pixel_data[i].a,
                .g = pixel_data[i].g*255/pixel_data[i].a,
                .b = pixel_data[i].b*255/pixel_data[i].a,
            };
        }
    }

    out->gamma = 0.45455;
    out->width = width;
    out->height = height;
    out->rgba_data = (unsigned char *)pixel_data;
    out->row_pointers = malloc(sizeof(out->row_pointers[0])*out->height);
    for(int i=0; i < out->height; i++) {
        out->row_pointers[i] = (unsigned char *)&pixel_data[width*i];
    }
    return SUCCESS;
}

#endif
