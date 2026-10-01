#pragma once
#include <CoreAudio/CoreAudio.h>
#include <QString>
namespace audio {
OSStatus createCoreAudioTap(const QString &deviceUID, UInt32 stream,
                            AudioObjectID *tap);
OSStatus destroyCoreAudioTap(AudioObjectID tap);
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
  decltype(&createCoreAudioTap) createTap = createCoreAudioTap;
  decltype(&destroyCoreAudioTap) destroyTap = destroyCoreAudioTap;
  decltype(&AudioHardwareCreateAggregateDevice) createAggregate =
      AudioHardwareCreateAggregateDevice;
  decltype(&AudioHardwareDestroyAggregateDevice) destroyAggregate =
      AudioHardwareDestroyAggregateDevice;
};
} // namespace audio
