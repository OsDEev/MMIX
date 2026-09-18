/*
 * Minimal USB constants for the UHCI host-controller driver.
 * Covers endpoint-0 control transfers (device/config/HID descriptors)
 * and HID boot-protocol keyboards.
 */
#ifndef MYUNIX_USB_H
#define MYUNIX_USB_H

#include <stdint.h>

/* USB packet IDs as stored in the UHCI TD token word (bits 7:0). */
#define USB_PID_IN    0x69
#define USB_PID_OUT   0xE1
#define USB_PID_SETUP 0x2D

/* Standard bmRequestType direction/recipient bits. */
#define USB_DIR_OUT   0x00
#define USB_DIR_IN    0x80
#define USB_TYPE_STD  0x00
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_DEV 0x00
#define USB_RECIP_IF  0x01

/* Standard requests. */
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE         0x0A
#define USB_REQ_GET_CONFIGURATION 0x08

/* Descriptor types. */
#define USB_DT_DEVICE   0x01
#define USB_DT_CONFIG   0x02
#define USB_DT_STRING   0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_HID      0x21

/* Interface classes we care about. */
#define USB_CLASS_HID 0x03
#define HID_SUBCLASS_BOOT 0x01
#define HID_PROTOCOL_KEYBOARD 0x01

#define USB_EP_IN 0x80
#define USB_EP_OUT 0x00

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* bit7: direction (1 = IN) */
    uint8_t  bmAttributes;      /* bits 1:0 transfer type */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

struct usb_hid_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bDescriptorTypeHID;
    uint16_t wDescriptorLength;
} __attribute__((packed));

/* HID boot-protocol keyboard report (8 bytes). */
struct hid_boot_kbd_report {
    uint8_t modifiers;   /* bit0 LCtrl, bit1 LShift, bit2 LAlt, ... */
    uint8_t reserved;
    uint8_t keys[6];     /* HID usage codes, 0 = empty */
} __attribute__((packed));

#endif /* MYUNIX_USB_H */