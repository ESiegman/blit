// Typesets a text file into a PDF of A4 pages (monospace, as a listing
// prints), for the README's PDF shot: clang -fobjc-arc pdf.m -framework
// AppKit -o pdf && ./pdf in.txt out.pdf
#import <AppKit/AppKit.h>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        NSString* text = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        if (!text) return 1;
        NSFont* font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
        NSDictionary* attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: NSColor.blackColor};
        NSAttributedString* all = [[NSAttributedString alloc] initWithString:text attributes:attrs];
        const CGRect page = CGRectMake(0, 0, 595, 842);
        const CGRect frame = CGRectInset(page, 56, 64);
        CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[2])];
        CGContextRef pdf = CGPDFContextCreateWithURL(url, &page, NULL);
        CTFramesetterRef setter = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)all);
        CFIndex at = 0;
        while (at < (CFIndex)all.length) {
            CGPDFContextBeginPage(pdf, NULL);
            CGPathRef path = CGPathCreateWithRect(frame, NULL);
            CTFrameRef f = CTFramesetterCreateFrame(setter, CFRangeMake(at, 0), path, NULL);
            CTFrameDraw(f, pdf);
            at += CTFrameGetVisibleStringRange(f).length;
            CFRelease(f);
            CGPathRelease(path);
            CGPDFContextEndPage(pdf);
        }
        CFRelease(setter);
        CGPDFContextClose(pdf);
        CGContextRelease(pdf);
    }
    return 0;
}
