/* TinyUSB declares usbh_app_driver_get_cb() as weak in usbh_pvt.h.  With the
 * ARM GCC build used here, a definition in the same translation unit inherits
 * that weak binding.  Keep the application table in flydigi_host.c and expose
 * it through this separate strong-symbol shim. */
#include <stdint.h>

extern void const *flydigi_app_driver_get_cb(uint8_t *driver_count);

void const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    return flydigi_app_driver_get_cb(driver_count);
}
