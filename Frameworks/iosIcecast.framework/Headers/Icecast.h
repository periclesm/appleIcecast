//
//  Icecast.h
//  Icecast
//
//  Created by Pericles Maravelakis on 31/12/2018.
//

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#endif

#if TARGET_OS_OSX
#import <Cocoa/Cocoa.h>
#endif

//! Project version number for Icecast.
FOUNDATION_EXPORT double IcecastVersionNumber;

//! Project version string for Icecast.
FOUNDATION_EXPORT const unsigned char IcecastVersionString[];

#include "shout.h"
#include "lame.h"
