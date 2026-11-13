// Loads a .saver bundle, runs it offline for N seconds and writes a PNG.
// usage: snapshot <bundle.saver> <out.png> <seconds> [width height]

#import <Cocoa/Cocoa.h>
#import <ScreenSaver/ScreenSaver.h>

@protocol DebugSaver
- (void)debugAdvance:(double)seconds;
- (NSImage *)debugSnapshot;
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc < 4) { fprintf(stderr, "usage: snapshot bundle out.png seconds [w h]\n"); return 2; }
        [NSApplication sharedApplication];
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        NSError *err = nil;
        if (![bundle loadAndReturnError:&err]) { NSLog(@"load failed: %@", err); return 1; }
        Class cls = bundle.principalClass;
        if (!cls) { NSLog(@"no principal class"); return 1; }
        CGFloat w = argc > 5 ? atof(argv[4]) : 1440, h = argc > 5 ? atof(argv[5]) : 900;
        ScreenSaverView *view = [[cls alloc] initWithFrame:NSMakeRect(0, 0, w, h) isPreview:NO];
        id<DebugSaver> dbg = (id<DebugSaver>)view;
        [dbg debugAdvance:atof(argv[3])];
        NSImage *img = [dbg debugSnapshot];
        if (!img) { NSLog(@"no image"); return 1; }
        CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:@(argv[2]) atomically:YES]) { NSLog(@"write failed"); return 1; }
        printf("wrote %s (%ldx%ld)\n", argv[2], (long)rep.pixelsWide, (long)rep.pixelsHigh);
    }
    return 0;
}
