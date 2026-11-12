#include "Touch.hpp"

#include <SDL3/SDL.h>

#import <AppKit/AppKit.h>

// A recognizer that never recognizes anything. It stays possible for as long
// as fingers are down, which keeps AppKit delivering every touch to it, and
// it claims nothing, so the view's clicks and the window's other handling
// carry on as before.
@interface QETouchRecognizer : NSGestureRecognizer
@end

@implementation QETouchRecognizer {
    NSMutableDictionary* ids_;
    uint64_t next_;
}

- (instancetype)init {
    if ((self = [super initWithTarget:nil action:nil])) {
        ids_ = [NSMutableDictionary dictionary];
        next_ = 1;
        self.allowedTouchTypes = NSTouchTypeMaskDirect;
        self.delaysPrimaryMouseButtonEvents = NO;
        self.delaysSecondaryMouseButtonEvents = NO;
        self.delaysOtherMouseButtonEvents = NO;
        self.delaysKeyEvents = NO;
        self.delaysMagnificationEvents = NO;
        self.delaysRotationEvents = NO;
    }

    return self;
}

- (void)report:(NSEvent*)event phase:(NSTouchPhase)phase as:(TouchEvent::Phase)out {
    NSView* view = self.view;
    const NSRect bounds = view.bounds;

    for (NSTouch* t in [event touchesMatchingPhase:phase inView:view]) {
        if (t.type != NSTouchTypeDirect) {
            continue;
        }

        NSNumber* n = ids_[t.identity];

        if (!n) {
            n = @(next_++);
            ids_[t.identity] = n;
        }

        const NSPoint p = [t locationInView:view];
        TouchEvent e;
        e.phase = out;
        e.id = n.unsignedLongLongValue;
        e.pos = ImVec2(float(p.x - bounds.origin.x),
                       float(view.isFlipped ? p.y - bounds.origin.y : NSMaxY(bounds) - p.y));
        touchPush(e);

        if (out == TouchEvent::Up) {
            [ids_ removeObjectForKey:t.identity];
        }
    }
}

- (void)touchesBeganWithEvent:(NSEvent*)event {
    [self report:event phase:NSTouchPhaseBegan as:TouchEvent::Down];
}

- (void)touchesMovedWithEvent:(NSEvent*)event {
    [self report:event phase:NSTouchPhaseMoved as:TouchEvent::Move];
}

- (void)touchesEndedWithEvent:(NSEvent*)event {
    [self report:event phase:NSTouchPhaseEnded as:TouchEvent::Up];
}

- (void)touchesCancelledWithEvent:(NSEvent*)event {
    [self report:event phase:NSTouchPhaseCancelled as:TouchEvent::Up];
}

@end

bool touchStart(SDL_Window* window) {
    if (@available(macOS 27.0, *)) {
        NSWindow* w = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                  SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);

        if (!w || !w.contentView) {
            return false;
        }

        [w.contentView addGestureRecognizer:[[QETouchRecognizer alloc] init]];
        return true;
    }

    return false;
}
