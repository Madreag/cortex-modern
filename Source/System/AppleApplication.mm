#include "AppleApplication.h"

#import <Foundation/Foundation.h>

void AppleRegisterApplicationDefaults(void) {
	@autoreleasepool {
		// AppKit decides window restoration while it handles the launch open-event, which SDL's first event pump
		// delivers, and after an unclean exit it asks there with a modal "reopen windows?" alert. SDL registers
		// ApplePersistenceIgnoreState only in applicationDidFinishLaunching, after that event, so every launch that
		// followed a killed or crashed one blocked on the alert. The game restores no windows: persistent UI is off.
		[[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePersistence": @NO}];
	}
}
