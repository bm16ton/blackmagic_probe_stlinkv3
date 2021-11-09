/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements a the USB Communications Device Class - Abstract
 * Control Model (CDC-ACM) as defined in CDC PSTN subclass 1.2.
 * A Device Firmware Upgrade (DFU 1.1) class interface is provided for
 * field firmware upgrade.
 *
 * The device's unique id is used as the USB serial number string.
 */

#include "general.h"
#include "cdcacm.h"
#include "usbuart.h"
#include "serialno.h"
#include "version.h"
#include <libopencm3/usb/dfu.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <platform_support.h>

#include <stdio.h>
#include <stdlib.h>
#include <libopencm3/usb/usbd.h>

#define LED_ON 1
#define LED_OFF 0

usbd_device * usbdev;

static int configured;

static void cdcacm_set_modem_state(usbd_device *dev, int iface, bool dsr, bool dcd);

static const struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0xEF,		/* Miscellaneous Device */
	.bDeviceSubClass = 2,		/* Common Class */
	.bDeviceProtocol = 1,		/* Interface Association */
	/* The USB specification requires that the control endpoint size for high
	 * speed devices (e.g., stlinkv3) is 64 bytes.
	 * Best to have its size set to 64 bytes in all cases. */
	.bMaxPacketSize0 = 64,
	.idVendor = 0x1D50,
	.idProduct = 0x6018,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

/* This notification endpoint isn't implemented. According to CDC spec its
 * optional, but its absence causes a NULL pointer dereference in Linux cdc_acm
 * driver. */
static const struct usb_endpoint_descriptor uart1_comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = (CDCACM_UART1_ENDPOINT + 1) | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 64,
	.bInterval =  4,
}};

static const struct usb_endpoint_descriptor uart1_data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_UART1_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 11,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_UART1_ENDPOINT | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 11,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) uart1_cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 2, /* SET_LINE_CODING supported */
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = CDCACM_UART1_INTERFACE,
		.bSubordinateInterface0 = CDCACM_UART1_INTERFACE + 1,
	 }
};

static const struct usb_interface_descriptor uart1_comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 4,

	.endpoint = uart1_comm_endp,

	.extra = &uart1_cdcacm_functional_descriptors,
	.extralen = sizeof(uart1_cdcacm_functional_descriptors)
}};

static const struct usb_interface_descriptor uart1_data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = uart1_data_endp,
}};

static const struct usb_iface_assoc_descriptor uart1_assoc = {
	.bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = 1,
	.bInterfaceCount = 2,
	.bFunctionClass = USB_CLASS_CDC,
	.bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
	.bFunctionProtocol = USB_CDC_PROTOCOL_AT,
	.iFunction = 0,
};

/* Serial ACM interface */
static const struct usb_endpoint_descriptor uart_comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = (CDCACM_UART_ENDPOINT + 1) | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 64,
	.bInterval =  1,
}};

static const struct usb_endpoint_descriptor uart_data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_UART_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_UART_ENDPOINT | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 1,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) uart_cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = CDCACM_UART_INTERFACE + 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 2, /* SET_LINE_CODING supported*/
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = CDCACM_UART_INTERFACE,
		.bSubordinateInterface0 = CDCACM_UART_INTERFACE + 1,
	 }
};

static const struct usb_interface_descriptor uart_comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 2,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 5,

	.endpoint = uart_comm_endp,

	.extra = &uart_cdcacm_functional_descriptors,
	.extralen = sizeof(uart_cdcacm_functional_descriptors)
}};

static const struct usb_interface_descriptor uart_data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 3,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = uart_data_endp,
}};

static const struct usb_iface_assoc_descriptor uart_assoc = {
	.bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = 3,
	.bInterfaceCount = 2,
	.bFunctionClass = USB_CLASS_CDC,
	.bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
	.bFunctionProtocol = USB_CDC_PROTOCOL_NONE,
	.iFunction = 0,
};

const struct usb_dfu_descriptor dfu_function = {
	.bLength = sizeof(struct usb_dfu_descriptor),
	.bDescriptorType = DFU_FUNCTIONAL,
	.bmAttributes = USB_DFU_CAN_DOWNLOAD | USB_DFU_WILL_DETACH,
	.wDetachTimeout = 255,
	.wTransferSize = 1024,
	.bcdDFUVersion = 0x011A,
};

const struct usb_interface_descriptor dfu_iface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 4,
	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = 0xFE,
	.bInterfaceSubClass = 1,
	.bInterfaceProtocol = 1,
	.iInterface = 6,

	.extra = &dfu_function,
	.extralen = sizeof(dfu_function),
};

static const struct usb_iface_assoc_descriptor dfu_assoc = {
	.bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = 0x04,
	.bInterfaceCount = 1,
	.bFunctionClass = 0xFE,
	.bFunctionSubClass = 1,
	.bFunctionProtocol = 1,
	.iFunction = 6,
};

const struct usb_endpoint_descriptor led_endpoint = {
     // The size of the endpoint descriptor in bytes: 7.
     .bLength = USB_DT_ENDPOINT_SIZE,
     // A value of 5 indicates that this describes an endpoint.
     .bDescriptorType = USB_DT_ENDPOINT,
     // Bit 7 indicates direction: 0 for OUT (to device) 1 for IN (to host).
     // Bits 6-4 must be set to 0.
     // Bits 3-0 indicate the endpoint number (zero is not allowed).
     // Here we define the IN side of endpoint 1.
     .bEndpointAddress = 0x85,
     // Bit 7-2 are only used in Isochronous mode, otherwise they should be
     // 0.
     // Bit 1-0: Indicates the mode of this endpoint.
     // 00: Control
     // 01: Isochronous
     // 10: Bulk
     // 11: Interrupt
     // Here we're using interrupt.
     .bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
     // Maximum packet size.
     .wMaxPacketSize = 64,
     // The frequency, in number of frames, that we're going to be sending
     // data. Here we're saying we're going to send data every millisecond.
     .bInterval = 11,
};

const struct usb_interface_descriptor led_iface = {
     // The size of an interface descriptor: 9
     .bLength = USB_DT_INTERFACE_SIZE,
     // A value of 4 specifies that this describes and interface.
     .bDescriptorType = USB_DT_INTERFACE,
     // The number for this interface. Starts counting from 0.
     .bInterfaceNumber = 5,
     // The number for this alternate setting for this interface.
     .bAlternateSetting = 0,
     // The number of endpoints in this interface.
     .bNumEndpoints = 1,
     // The interface class for this interface is DATA, indicated by 10.
     .bInterfaceClass = 0,
     // There are no subclasses defined for the data class so it must be zero.
     .bInterfaceSubClass = 0, /* boot */
     // We are not using any class specific protocols for data so this is set to
     // zero
     .bInterfaceProtocol = 0, /* keyboard */
     // A string representing this interface. Zero means not provided.
     .iInterface = 0,
     // A pointer to the array of endpoints in this interface.
     .endpoint = &led_endpoint,

     .extra = NULL,
     .extralen = 0,
};

/*SLCAN interface */
static const struct usb_endpoint_descriptor slcan_comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = (CDCACM_SLCAN_ENDPOINT + 1) | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 64,
	.bInterval =  10,
}};

static const struct usb_endpoint_descriptor slcan_data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_SLCAN_ENDPOINT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 10,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = CDCACM_SLCAN_ENDPOINT | USB_REQ_TYPE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 512,
	.bInterval = 10,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) slcan_cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = CDCACM_SLCAN_INTERFACE + 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 2, /* SET_LINE_CODING supported */
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = CDCACM_SLCAN_INTERFACE,
		.bSubordinateInterface0 = CDCACM_SLCAN_INTERFACE + 1,
	 }
};

static const struct usb_interface_descriptor slcan_comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 6,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 8,

	.endpoint = slcan_comm_endp,

	.extra = &slcan_cdcacm_functional_descriptors,
	.extralen = sizeof(slcan_cdcacm_functional_descriptors)
}};

static const struct usb_interface_descriptor slcan_data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 7,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = slcan_data_endp,
}};

static const struct usb_iface_assoc_descriptor slcan_assoc = {
	.bLength = USB_DT_INTERFACE_ASSOCIATION_SIZE,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = CDCACM_SLCAN_INTERFACE,
	.bInterfaceCount = 2,
	.bFunctionClass = USB_CLASS_CDC,
	.bFunctionSubClass = USB_CDC_SUBCLASS_ACM,
	.bFunctionProtocol = USB_CDC_PROTOCOL_NONE,
	.iFunction = 0,
};


static const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.iface_assoc = &uart1_assoc,
	.altsetting = uart1_comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = uart1_data_iface,
}, {
	.num_altsetting = 1,
	.iface_assoc = &uart_assoc,
	.altsetting = uart_comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = uart_data_iface,
}, {
    .num_altsetting = 1,
    .altsetting = &led_iface,
}, {
	.num_altsetting = 1,
	.iface_assoc = &dfu_assoc,
	.altsetting = &dfu_iface,
}, {
	.num_altsetting = 1,
	.iface_assoc = &slcan_assoc,
	.altsetting = slcan_comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = slcan_data_iface,
}};

static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 8,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};
static char serial_no[DFU_SERIAL_LENGTH];

#define BOARD_IDENT "Black Magic Probe " PLATFORM_IDENT FIRMWARE_VERSION
#define DFU_IDENT   "Black Magic DFU " PLATFORM_IDENT FIRMWARE_VERSION

static const char *usb_strings[] = {
	"Black Sphere Technologies",
	BOARD_IDENT,
	serial_no,
	"Black Magic GDB Server",
	"Black Magic UART Port",
	DFU_IDENT,
	"led class",
	"Black Magic SLCAN",
};

static void dfu_detach_complete(usbd_device *dev, struct usb_setup_data *req)
{
	(void)dev;
	(void)req;

	platform_request_boot();

	/* Reset core to enter bootloader */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
	scb_reset_core();
#endif
}

//uint8_t usbd_control_buffer[128];

static enum usbd_request_return_codes control_request(usbd_device *dev, struct usb_setup_data *req,
                           uint8_t **buf, uint16_t *len,
                           void (**complete)(usbd_device *,
                                             struct usb_setup_data *))
{
   (void)complete;
   (void)dev;

   if ((req->bmRequestType & 0x7F) != USB_REQ_TYPE_VENDOR)
     return 0;

   (*len) = 1;
   (*buf)[0] = 1; //success

   if (req->bRequest == LED_ON)
     {
        gpio_set(GPIOA, GPIO10);
     }
   else if (req->bRequest == LED_OFF)
     {
        gpio_clear(GPIOA, GPIO10);
     }
   if (req->bRequest == LED_ON)
     {
        gpio_set(GPIOA, GPIO10);
     }
   else if (req->bRequest == LED_OFF)
     {
        gpio_clear(GPIOA, GPIO10);
     }
   else
     {
        (*buf)[0] = -1; // FAILURE
     }

   return 1;
}

static enum usbd_request_return_codes  cdcacm_control_request(usbd_device *dev,
		struct usb_setup_data *req, uint8_t **buf, uint16_t *len,
		void (**complete)(usbd_device *dev, struct usb_setup_data *req))
{
	(void)dev;
	(void)complete;
	(void)buf;
	(void)len;

	switch(req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE:
		cdcacm_set_modem_state(dev, req->wIndex, true, true);
		return USBD_REQ_HANDLED;
	case USB_CDC_REQ_SET_LINE_CODING:
		if(*len < sizeof(struct usb_cdc_line_coding))
			return USBD_REQ_NOTSUPP;

		switch(req->wIndex) {
		case CDCACM_UART_INTERFACE:
			usbuart_set_line_coding((struct usb_cdc_line_coding*)*buf);
			return USBD_REQ_HANDLED;
		case CDCACM_SLCAN_INTERFACE:
			return USBD_REQ_HANDLED; /* Ignore on UART1 Port */
		default:
			return USBD_REQ_NOTSUPP;
		}
	case DFU_GETSTATUS:
		if(req->wIndex == 0x04) {
			(*buf)[0] = DFU_STATUS_OK;
			(*buf)[1] = 0;
			(*buf)[2] = 0;
			(*buf)[3] = 0;
			(*buf)[4] = STATE_APP_IDLE;
			(*buf)[5] = 0;	/* iString not used here */
			*len = 6;

			return USBD_REQ_HANDLED;
		}
		return USBD_REQ_NOTSUPP;
	case DFU_DETACH:
		if(req->wIndex ==  0x04) {
			*complete = dfu_detach_complete;
			return USBD_REQ_HANDLED;
		}
		return USBD_REQ_NOTSUPP;
	}
	return USBD_REQ_NOTSUPP;
}

int cdcacm_get_config(void)
{
	return configured;
}

static void cdcacm_set_modem_state(usbd_device *dev, int iface, bool dsr, bool dcd)
{
	char buf[10];
	struct usb_cdc_notification *notif = (void*)buf;
	/* We echo signals back to host as notification */
	notif->bmRequestType = 0xA1;
	notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
	notif->wValue = 0;
	notif->wIndex = iface;
	notif->wLength = 2;
	buf[8] = (dsr ? 2 : 0) | (dcd ? 1 : 0);
	buf[9] = 0;
	usbd_ep_write_packet(dev, 0x82 + iface, buf, 10);
}

extern void slcan_usb_out_cb(usbd_device *dev, uint8_t ep);
extern void slcan_usb_in_cb(usbd_device *dev, uint8_t ep);


static void cdcacm_set_config(usbd_device *dev, uint16_t wValue)
{
	configured = wValue;

	/* UART1 interface */
	usbd_ep_setup(dev, CDCACM_UART1_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
	              512, usbuart1_usb_out_cb);
	usbd_ep_setup(dev, CDCACM_UART1_ENDPOINT | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_BULK, 512, NULL);
	usbd_ep_setup(dev, (CDCACM_UART1_ENDPOINT + 1) | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_INTERRUPT, 64, NULL);

	/* Serial interface */
	usbd_ep_setup(dev, CDCACM_UART_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
	              512, usbuart_usb_out_cb);
	usbd_ep_setup(dev, CDCACM_UART_ENDPOINT | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_BULK,
				  512, usbuart_usb_in_cb);
	usbd_ep_setup(dev, (CDCACM_UART_ENDPOINT + 1) | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_INTERRUPT, 64, NULL);

	/* SLCAN interface */
	usbd_ep_setup(dev, CDCACM_SLCAN_ENDPOINT, USB_ENDPOINT_ATTR_BULK,
	              CDCACM_PACKET_SIZE, slcan_usb_out_cb);
	usbd_ep_setup(dev, CDCACM_SLCAN_ENDPOINT | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_BULK, CDCACM_PACKET_SIZE, NULL);
	usbd_ep_setup(dev, (CDCACM_SLCAN_ENDPOINT + 1) | USB_REQ_TYPE_IN,
				  USB_ENDPOINT_ATTR_INTERRUPT, 64, NULL);


	usbd_register_control_callback(dev,
			USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
			USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
			cdcacm_control_request);

	/* Notify the host that DCD is asserted.
	 * Allows the use of /dev/tty* devices on *BSD/MacOS
	 */
	cdcacm_set_modem_state(dev, 0, true, true);
	cdcacm_set_modem_state(dev, 2, true, true);
	cdcacm_set_modem_state(dev, 5, true, true);

	   //we are not going to use this 0x81, EP0 is good enough
   //usbd_ep_setup(dev, 0x81, USB_ENDPOINT_ATTR_INTERRUPT, 9, NULL);

	usbd_ep_setup(dev, 0x85, USB_ENDPOINT_ATTR_INTERRUPT, 9, NULL);

    usbd_register_control_callback(dev,
                                  USB_REQ_TYPE_VENDOR,//USB_REQ_TYPE_STANDARD | USB_REQ_TYPE_INTERFACE,
                                  USB_REQ_TYPE_TYPE, /// | USB_REQ_TYPE_RECIPIENT,
                                  control_request);
}


/* We need a special large control buffer for this device: */

uint8_t usbd_control_buffer[512];

void cdcacm_init(void)
{
	void exti15_10_isr(void);

	serial_no_read(serial_no);

	usbdev = usbd_init(&USB_DRIVER, &dev_desc, &config, usb_strings,
			    sizeof(usb_strings)/sizeof(char *),
			    usbd_control_buffer, sizeof(usbd_control_buffer));

	usbd_register_set_config_callback(usbdev, cdcacm_set_config);

	nvic_set_priority(USB_IRQ, IRQ_PRI_USB);
	nvic_enable_irq(USB_IRQ);
}

void USB_ISR(void)
{
	usbd_poll(usbdev);
}
