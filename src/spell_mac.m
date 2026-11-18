// Spelling on a Mac: the system's spell checker, in English. (The C++ side
// is spell.cpp; this is the part only Objective-C can say.)
#import <AppKit/AppKit.h>

#include <string.h>

static NSString* word_of(const char* s, size_t n) { return [[NSString alloc] initWithBytes:s length:n encoding:NSUTF8StringEncoding]; }

int blit_spell_ready(void) {
    @autoreleasepool {
        NSSpellChecker* c = [NSSpellChecker sharedSpellChecker];
        return c && [[c availableLanguages] count] > 0;
    }
}

int blit_spell_ok(const char* s, size_t n) {
    @autoreleasepool {
        NSString* w = word_of(s, n);
        if (!w) return 1;
        const NSRange r = [[NSSpellChecker sharedSpellChecker] checkSpellingOfString:w startingAt:0 language:@"en" wrap:NO inSpellDocumentWithTag:0 wordCount:NULL];
        return r.location == NSNotFound;
    }
}

// Each guess, written into out (each ended by a 0), as many as fit; how many.
int blit_spell_suggest(const char* s, size_t n, char* out, size_t room) {
    @autoreleasepool {
        NSString* w = word_of(s, n);
        if (!w) return 0;
        NSArray<NSString*>* g = [[NSSpellChecker sharedSpellChecker] guessesForWordRange:NSMakeRange(0, [w length]) inString:w language:@"en" inSpellDocumentWithTag:0];
        int count = 0;
        size_t at = 0;
        for (NSString* x in g) {
            const char* u = [x UTF8String];
            const size_t len = strlen(u);
            if (at + len + 1 > room || count == 12) break;
            memcpy(out + at, u, len + 1);
            at += len + 1;
            ++count;
        }
        return count;
    }
}

void blit_spell_learn(const char* s, size_t n) {
    @autoreleasepool {
        NSString* w = word_of(s, n);
        if (w) [[NSSpellChecker sharedSpellChecker] learnWord:w];
    }
}
