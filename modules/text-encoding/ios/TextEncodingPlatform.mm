/*
 * Apple platform layer for react-native-quickjs-text-encoding.
 *
 * Implements the TextEncodingPlatform interface using Foundation. Nothing needs
 * to link ICU: NSLocale, NSDateFormatter and NSNumberFormatter already carry
 * the data, which is the same approach Hermes takes for Intl.
 */

#import <Foundation/Foundation.h>

#include "TextEncodingPlatform.h"

namespace {

class ApplePlatform : public text_encoding::Platform {
 public:
  std::string deviceLocale() override {
    NSString *tag = [[NSLocale currentLocale] localeIdentifier];
    return tag ? std::string([tag UTF8String]) : std::string("en-US");
  }
};

ApplePlatform gPlatform;

}  // namespace

/*
 * Registered at load time so the platform is in place before the runtime is
 * created. A +load method runs when the image is loaded, which is earlier than
 * any React Native lifecycle hook and therefore earlier than any JavaScript.
 */
@interface TextEncodingPlatformRegistrar : NSObject
@end

@implementation TextEncodingPlatformRegistrar
+ (void)load {
  text_encoding::setPlatform(&gPlatform);
}
@end
