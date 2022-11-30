#include "host.h"

#define FUNC(enum_name,...) ENUM_TO_STR_FUNC(enum_name){switch(val)__VA_ARGS__ return #enum_name "_unk";}

FUNC(libusb_class_code, {
  #undef CASE
  #define CASE(x) case LIBUSB_CLASS_##x: return #x;
  CASE(PER_INTERFACE)
  CASE(AUDIO)
  CASE(COMM)
  CASE(HID)
  CASE(PHYSICAL)
  CASE(IMAGE)
  CASE(PRINTER)
  CASE(MASS_STORAGE)
  CASE(HUB)
  CASE(DATA)
  CASE(SMART_CARD)
  CASE(CONTENT_SECURITY)
  CASE(VIDEO)
  CASE(PERSONAL_HEALTHCARE)
  CASE(DIAGNOSTIC_DEVICE)
  CASE(WIRELESS)
  CASE(MISCELLANEOUS)
  CASE(APPLICATION)
  CASE(VENDOR_SPEC)
})

FUNC(libusb_descriptor_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_DT_##x: return #x;
  CASE(DEVICE)
  CASE(CONFIG)
  CASE(STRING)
  CASE(INTERFACE)
  CASE(ENDPOINT)
  CASE(BOS)
  CASE(DEVICE_CAPABILITY)
  CASE(HID)
  CASE(REPORT)
  CASE(PHYSICAL)
  CASE(HUB)
  CASE(SUPERSPEED_HUB)
  CASE(SS_ENDPOINT_COMPANION)
})

FUNC(libusb_endpoint_direction, {
  #undef CASE
  #define CASE(x) case LIBUSB_ENDPOINT_##x: return #x;
  CASE(OUT)
  CASE(IN)
})

FUNC(libusb_endpoint_transfer_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_ENDPOINT_TRANSFER_TYPE_##x: return #x;
  CASE(CONTROL)
  CASE(ISOCHRONOUS)
  CASE(BULK)
  CASE(INTERRUPT)
})

FUNC(libusb_standard_request, {
  #undef CASE
  #define CASE(x) case LIBUSB_REQUEST_##x: return #x;
  CASE(GET_STATUS)
  CASE(CLEAR_FEATURE)
  CASE(SET_FEATURE)
  CASE(SET_ADDRESS)
  CASE(GET_DESCRIPTOR)
  CASE(SET_DESCRIPTOR)
  CASE(GET_CONFIGURATION)
  CASE(SET_CONFIGURATION)
  CASE(GET_INTERFACE)
  CASE(SET_INTERFACE)
  CASE(SYNCH_FRAME)
  CASE(SET_SEL)
  case LIBUSB_SET_ISOCH_DELAY: return "SET_ISOCH_DELAY";
})

FUNC(libusb_request_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_REQUEST_TYPE_##x: return #x;
  CASE(STANDARD)
  CASE(CLASS)
  CASE(VENDOR)
  CASE(RESERVED)
})

FUNC(libusb_request_recipient, {
  #undef CASE
  #define CASE(x) case LIBUSB_RECIPIENT_##x: return #x;
  CASE(DEVICE)
  CASE(INTERFACE)
  CASE(ENDPOINT)
  CASE(OTHER)
})

FUNC(libusb_iso_sync_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_ISO_SYNC_TYPE_##x: return #x;
  CASE(NONE)
  CASE(ASYNC)
  CASE(ADAPTIVE)
  CASE(SYNC)
})

FUNC(libusb_iso_usage_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_ISO_USAGE_TYPE_##x: return #x;
  CASE(DATA)
  CASE(FEEDBACK)
  CASE(IMPLICIT)
})

FUNC(libusb_supported_speed, {
  #undef CASE
  #define CASE(x) case LIBUSB_##x: return #x;
  CASE(LOW_SPEED_OPERATION)
  CASE(FULL_SPEED_OPERATION)
  CASE(HIGH_SPEED_OPERATION)
  CASE(SUPER_SPEED_OPERATION)
})

FUNC(libusb_bos_type, {
  #undef CASE
  #define CASE(x) case LIBUSB_BT_##x: return #x;
  CASE(WIRELESS_USB_DEVICE_CAPABILITY)
  CASE(USB_2_0_EXTENSION)
  CASE(SS_USB_DEVICE_CAPABILITY)
  CASE(CONTAINER_ID)
})

FUNC(libusb_speed, {
  #undef CASE
  #define CASE(x) case LIBUSB_SPEED_##x: return #x;
  CASE(UNKNOWN)
  CASE(LOW)
  CASE(FULL)
  CASE(HIGH)
  CASE(SUPER)
  CASE(SUPER_PLUS)
})
