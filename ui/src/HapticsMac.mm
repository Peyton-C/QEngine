#import <AppKit/AppKit.h>

void encoderHaptic() {
    [NSHapticFeedbackManager.defaultPerformer performFeedbackPattern:NSHapticFeedbackPatternGeneric
                                                     performanceTime:NSHapticFeedbackPerformanceTimeNow];
}
