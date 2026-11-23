#import <AppKit/AppKit.h>

void detentHaptic() {
    [NSHapticFeedbackManager.defaultPerformer performFeedbackPattern:NSHapticFeedbackPatternGeneric
                                                     performanceTime:NSHapticFeedbackPerformanceTimeNow];
}
