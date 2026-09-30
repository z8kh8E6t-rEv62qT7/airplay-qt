#import <Cocoa/Cocoa.h>
#include <cstdio>

// Launch the actual packaged executable, observe a visible window, then request
// normal application termination. No test switch enters the shipping binary.
int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 3) return 2;
    NSString *bundle = [NSString stringWithUTF8String:argv[1]];
    NSString *log = [NSString stringWithUTF8String:argv[2]];
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle *output = [NSFileHandle fileHandleForWritingAtPath:log];
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:
        [bundle stringByAppendingPathComponent:@"Contents/MacOS/AirPlayQt"]];
    NSMutableDictionary *environment = [[[NSProcessInfo processInfo] environment] mutableCopy];
    for (NSString *key in [environment allKeys])
      if ([key hasPrefix:@"QT_"] || [key hasPrefix:@"DYLD_"])
        [environment removeObjectForKey:key];
    environment[@"DYLD_PRINT_LIBRARIES"] = @"1";
    environment[@"QT_DEBUG_PLUGINS"] = @"1";
    task.environment = environment;
    task.standardOutput = output;
    task.standardError = output;
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
      std::fprintf(stderr, "launch: %s\n", error.description.UTF8String);
      return 1;
    }
    bool visible = false;
    NSRunningApplication *application = nil;
    const auto deadline = [NSDate dateWithTimeIntervalSinceNow:20];
    while (task.running && [deadline timeIntervalSinceNow] > 0) {
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
      application = [NSRunningApplication runningApplicationWithProcessIdentifier:task.processIdentifier];
      CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
      for (NSDictionary *window in (__bridge NSArray *)windows) {
        if ([window[(id)kCGWindowOwnerPID] intValue] == task.processIdentifier &&
            [window[(id)kCGWindowLayer] intValue] == 0) visible = true;
      }
      if (windows) CFRelease(windows);
      if (visible && application.finishedLaunching) break;
    }
    bool requested = application && [application terminate];
    const auto stopDeadline = [NSDate dateWithTimeIntervalSinceNow:10];
    while (task.running && [stopDeadline timeIntervalSinceNow] > 0)
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    if (task.running) {
      [task terminate];
      std::fprintf(stderr, "application did not quit normally\n");
      return 1;
    }
    [output closeFile];
    if (!visible || !requested || task.terminationReason != NSTaskTerminationReasonExit ||
        task.terminationStatus != 0) {
      std::fprintf(stderr, "window=%d quit=%d exit=%d\n", visible, requested, task.terminationStatus);
      return 1;
    }
    std::puts("packaged app: visible window and normal exit 0");
    return 0;
  }
}
