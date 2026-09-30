/* Check whether macOS exposes the bridge to its Force Feedback API.
 * Build: clang ff_probe.c -framework CoreFoundation -framework IOKit -framework ForceFeedback -o ff_probe
 * Run:   ./ff_probe [vid_hex pid_hex]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <ForceFeedback/ForceFeedback.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>

static int property_number(IOHIDDeviceRef device, CFStringRef key)
{
    CFNumberRef value = IOHIDDeviceGetProperty(device, key);
    int number = -1;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(value, kCFNumberIntType, &number);
    return number;
}

int main(int argc, char **argv)
{
    int vid = 0x1209, pid = 0x0001;
    if (argc == 3) {
        vid = (int)strtol(argv[1], NULL, 16);
        pid = (int)strtol(argv[2], NULL, 16);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [vid_hex pid_hex]\n", argv[0]);
        return 2;
    }

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return 2;
    IOHIDManagerSetDeviceMatching(manager, NULL);
    IOReturn open_result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    CFSetRef devices = open_result == kIOReturnSuccess ? IOHIDManagerCopyDevices(manager) : NULL;
    if (!devices) {
        fprintf(stderr, "HID device enumeration failed: 0x%08x\n", open_result);
        CFRelease(manager);
        return 2;
    }

    CFIndex count = CFSetGetCount(devices);
    const void **values = calloc((size_t)count, sizeof(*values));
    if (!values) {
        CFRelease(devices);
        CFRelease(manager);
        return 2;
    }
    CFSetGetValues(devices, values);
    int matched = 0;
    for (CFIndex i = 0; i < count; ++i) {
        IOHIDDeviceRef device = (IOHIDDeviceRef)values[i];
        if (property_number(device, CFSTR(kIOHIDVendorIDKey)) != vid ||
            property_number(device, CFSTR(kIOHIDProductIDKey)) != pid)
            continue;
        ++matched;
        IOReturn device_open = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
        io_service_t service = IOHIDDeviceGetService(device);
        HRESULT result = service ? FFIsForceFeedback(service) : FFERR_INVALIDPARAM;
        const char *state = result == FF_OK ? "supported" :
                            result == FFERR_NOINTERFACE ? "unsupported" : "error";
        printf("%04x:%04x usage=%04x:%04x open=0x%08x service=%u FFIsForceFeedback=0x%08x %s\n",
               vid, pid,
               property_number(device, CFSTR(kIOHIDPrimaryUsagePageKey)),
               property_number(device, CFSTR(kIOHIDPrimaryUsageKey)),
               device_open, service, (unsigned)result, state);
        if (result == FF_OK) {
            FFDeviceObjectReference ff_device = NULL;
            HRESULT create = FFCreateDevice(service, &ff_device);
            printf("  FFCreateDevice=0x%08x\n", (unsigned)create);
            if (create == FF_OK && ff_device) {
                FFCAPABILITIES caps = {0};
                HRESULT capabilities = FFDeviceGetForceFeedbackCapabilities(ff_device, &caps);
                printf("  capabilities=0x%08x axes=%u customForce=%s\n",
                       (unsigned)capabilities, capabilities == FF_OK ? caps.numFfAxes : 0,
                       capabilities == FF_OK && (caps.supportedEffects & FFCAP_ET_CUSTOMFORCE) ?
                           "yes" : "no");
                FFReleaseDevice(ff_device);
            }
        }
        if (device_open == kIOReturnSuccess)
            IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    }
    free(values);
    CFRelease(devices);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    if (!matched) fprintf(stderr, "no HID device matched %04x:%04x\n", vid, pid);
    return matched ? 0 : 1;
}
