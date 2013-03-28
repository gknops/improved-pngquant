//
//  NSBitmapImageRep+PNG8Bit.h
//  Pnm8bit
//
//  Created by Gerd Knops on 2/26/13.
//  Copyright (c) 2013 BITart.com. All rights reserved.
//
#import <Cocoa/Cocoa.h>

#define USE_NSLOG

#include "pngquant.h"

@interface NSBitmapImageRep (PNG8Bit)

- (NSData *)png8data;
- (NSData *)png8dataWithOptions:(PNGQuantOptions *)optionPtr;

@end
