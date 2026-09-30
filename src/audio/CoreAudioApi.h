#pragma once
#include <CoreAudio/CoreAudio.h>
namespace audio {
// Native calls are injected at the OS boundary for device/lifecycle tests.
struct CoreAudioApi {
  decltype(&AudioObjectGetPropertyDataSize) size =
      AudioObjectGetPropertyDataSize;
  decltype(&AudioObjectGetPropertyData) get = AudioObjectGetPropertyData;
  decltype(&AudioObjectAddPropertyListener) listen =
      AudioObjectAddPropertyListener;
  decltype(&AudioObjectRemovePropertyListener) unlisten =
      AudioObjectRemovePropertyListener;
  decltype(&AudioDeviceCreateIOProcID) create = AudioDeviceCreateIOProcID;
  decltype(&AudioDeviceDestroyIOProcID) destroy = AudioDeviceDestroyIOProcID;
  decltype(&AudioDeviceStart) start = AudioDeviceStart;
  decltype(&AudioDeviceStop) stop = AudioDeviceStop;
};
} // namespace audio
