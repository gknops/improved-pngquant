//
//  NSBitmapImageRep+PNG8Bit.h
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
#define USE_NSLOG

#include "pngquant.h"

@interface NSBitmapImageRep (PNG8Bit)

//*****************************************************************************
// API
//*****************************************************************************
- (NSData *)png8data;
- (NSData *)png8dataWithOptions:(PNGQuantOptions *)optionPtr;

@end

// BaCPP checksum: 2028
