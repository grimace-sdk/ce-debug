#ifndef USBSERIAL_EVENTS_H
#define USBSERIAL_EVENTS_H

#include <windows.h>

#define IOCTL_USBSERIAL_EVENTS 0x00222004
#define USB_SERIAL_EVENT_LIMIT 64

enum usb_serial_event_code {
    USB_SERIAL_EVENT_NOTIFY = 1,
    USB_SERIAL_EVENT_SETUP = 2,
    USB_SERIAL_EVENT_PIPE = 3,
    USB_SERIAL_EVENT_RX_ISSUE = 4,
    USB_SERIAL_EVENT_RX_COMPLETE = 5,
    USB_SERIAL_EVENT_TX_ISSUE = 6,
    USB_SERIAL_EVENT_TX_COMPLETE = 7,
    USB_SERIAL_EVENT_CONTROL_ISSUE = 8,
    USB_SERIAL_EVENT_CONTROL_COMPLETE = 9,
    USB_SERIAL_EVENT_READ = 10,
    USB_SERIAL_EVENT_TX_TIMEOUT = 11,
    USB_SERIAL_EVENT_RX_SKIPPED = 12,
    USB_SERIAL_EVENT_CLEAR_STALL = 13,
};

typedef struct {
    DWORD sequence;
    DWORD tick;
    DWORD code;
    DWORD first;
    DWORD second;
} usb_serial_event;

typedef struct {
    DWORD next_sequence;
    usb_serial_event events[USB_SERIAL_EVENT_LIMIT];
} usb_serial_event_log;

#endif
