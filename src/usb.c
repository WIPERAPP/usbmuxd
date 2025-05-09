/*
 * usb.c
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2009-2020 Martin Szulecki <martin.szulecki@libimobiledevice.org>
 * Copyright (C) 2014 Mikkel Kamstrup Erlandsen <mikkel.kamstrup@xamarin.com>
 * Modified 25/03/2025 by Przemyslaw Muszynski
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <libusb.h>

#include <libimobiledevice-glue/collection.h>

#include "usb.h"
#include "log.h"
#include "device.h"
#include "utils.h"

#if (defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)) || (defined(LIBUSBX_API_VERSION) && (LIBUSBX_API_VERSION >= 0x01000102))
#define HAVE_LIBUSB_HOTPLUG_API 1
#endif

// interval for device connection/disconnection polling, in milliseconds
// we need this because there is currently no asynchronous device discovery mechanism in libusb
#define DEVICE_POLL_TIME 1000

// Number of parallel bulk transfers we have running for reading data from the device.
// Older versions of usbmuxd kept only 1, which leads to a mostly dormant USB port.
// 3 seems to be an all round sensible number - giving better read perf than
// Apples usbmuxd, at least.
#define NUM_RX_LOOPS 3

struct usb_device {
	libusb_device_handle *handle;
	uint8_t bus, address;
	char serial[256];
	int alive;
	uint8_t interface, ep_in, ep_out;
	struct collection rx_xfers;
	struct collection tx_xfers;
	int wMaxPacketSize;
	uint64_t speed;
	struct libusb_device_descriptor devdesc;
};

struct mode_context {
	struct libusb_device* dev;
	uint8_t bus, address;
	uint8_t bRequest;
	uint16_t wValue, wIndex, wLength;
	unsigned int timeout;
};

static struct collection device_list;

static struct timeval next_dev_poll_time;

static int devlist_failures;
static int device_polling;
static int device_hotplug = 1;

static void usb_disconnect(struct usb_device *dev)
{
	if(!dev->handle) {
		return;
	}

	// kill the rx xfer and tx xfers and try to make sure the callbacks
	// get called before we free the device
	FOREACH(struct libusb_transfer *xfer, &dev->rx_xfers) {
		usbmuxd_log(LL_DEBUG, "usb_disconnect: cancelling RX xfer %p", xfer);
		libusb_cancel_transfer(xfer);
	} ENDFOREACH

	FOREACH(struct libusb_transfer *xfer, &dev->tx_xfers) {
		usbmuxd_log(LL_DEBUG, "usb_disconnect: cancelling TX xfer %p", xfer);
		libusb_cancel_transfer(xfer);
	} ENDFOREACH

	// Busy-wait until all xfers are closed
	//while(collection_count(&dev->rx_xfers) || collection_count(&dev->tx_xfers)) {
	//	struct timeval tv;
	//	int res;
//
	//	tv.tv_sec = 0;
	//	tv.tv_usec = 1000;
	//	if((res = libusb_handle_events_timeout(NULL, &tv)) < 0) {
	//		usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout for usb_disconnect failed: %s", libusb_error_name(res));
	//		break;
	//	}
	//}
	    // Wait for cancellations to complete but with a timeout
    int timeout_ms = 100; // 100ms timeout
    int wait_step_us = 1000; // 1ms steps
    int max_iterations = timeout_ms * 1000 / wait_step_us;
    int iterations = 0;
		while((collection_count(&dev->rx_xfers) || collection_count(&dev->tx_xfers)) && iterations < max_iterations) {
        struct timeval tv;
        int res;

        tv.tv_sec = 0;
        tv.tv_usec = wait_step_us;
        if((res = libusb_handle_events_timeout(NULL, &tv)) < 0) {
            usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout for usb_disconnect failed: %s", libusb_error_name(res));
            break;
        }
        iterations++;
    }

	// If we still have pending transfers after timeout, force cleanup
    if(collection_count(&dev->rx_xfers) || collection_count(&dev->tx_xfers)) {
        usbmuxd_log(LL_WARNING, "Some transfers failed to complete during disconnect for device %d-%d - forcing cleanup", 
                    dev->bus, dev->address);
                    
        // Force cleanup of any remaining transfers
        FOREACH(struct libusb_transfer *xfer, &dev->rx_xfers) {
        if(xfer->buffer)
            free(xfer->buffer);
            libusb_free_transfer(xfer);
        } ENDFOREACH
        collection_init(&dev->rx_xfers); // reinitialize to clear all entries
        
        FOREACH(struct libusb_transfer *xfer, &dev->tx_xfers) {
            if(xfer->buffer)
                free(xfer->buffer);
            libusb_free_transfer(xfer);
        } ENDFOREACH
        collection_init(&dev->tx_xfers); // reinitialize to clear all entries
    }

	collection_free(&dev->tx_xfers);
	collection_free(&dev->rx_xfers);
	libusb_release_interface(dev->handle, dev->interface);
	libusb_close(dev->handle);
	dev->handle = NULL;
	collection_remove(&device_list, dev);
	free(dev);
}

static void reap_dead_devices(void) {
	FOREACH(struct usb_device *usbdev, &device_list) {
		if(!usbdev->alive) {
			device_remove(usbdev);
			usb_disconnect(usbdev);
		}
	} ENDFOREACH
}

// Callback from write operation
static void tx_callback(struct libusb_transfer *xfer)
{
	struct usb_device *dev = xfer->user_data;
	usbmuxd_log(LL_SPEW, "TX callback dev %d-%d len %d -> %d status %d", dev->bus, dev->address, xfer->length, xfer->actual_length, xfer->status);
	if(xfer->status != LIBUSB_TRANSFER_COMPLETED) {
		switch(xfer->status) {
			case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
			case LIBUSB_TRANSFER_ERROR:
				// funny, this happens when we disconnect the device while waiting for a transfer, sometimes
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to error or disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_TIMED_OUT:
				usbmuxd_log(LL_ERROR, "TX transfer timed out for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_CANCELLED:
				usbmuxd_log(LL_DEBUG, "Device %d-%d TX transfer cancelled", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_STALL:
				usbmuxd_log(LL_ERROR, "TX transfer stalled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_NO_DEVICE:
				// other times, this happens, and also even when we abort the transfer after device removal
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_OVERFLOW:
				usbmuxd_log(LL_ERROR, "TX transfer overflow for device %d-%d", dev->bus, dev->address);
				break;
			// and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
			default:
				// this should never be reached.
				break;
		}
		// we can't usb_disconnect here due to a deadlock, so instead mark it as dead and reap it after processing events
		// we'll do device_remove there too
		dev->alive = 0;
	}
	if(xfer->buffer)
		free(xfer->buffer);
	collection_remove(&dev->tx_xfers, xfer);
	libusb_free_transfer(xfer);
}

int usb_send(struct usb_device *dev, const unsigned char *buf, int length)
{
	int res;
	struct libusb_transfer *xfer = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(xfer, dev->handle, dev->ep_out, (void*)buf, length, tx_callback, dev, 0);
	if((res = libusb_submit_transfer(xfer)) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit TX transfer %p len %d to device %d-%d: %s", buf, length, dev->bus, dev->address, libusb_error_name(res));
		libusb_free_transfer(xfer);
		return res;
	}
	collection_add(&dev->tx_xfers, xfer);
	if (length % dev->wMaxPacketSize == 0) {
		usbmuxd_log(LL_DEBUG, "Send ZLP");
		// Send Zero Length Packet
		xfer = libusb_alloc_transfer(0);
		void *buffer = malloc(1);
		libusb_fill_bulk_transfer(xfer, dev->handle, dev->ep_out, buffer, 0, tx_callback, dev, 0);
		if((res = libusb_submit_transfer(xfer)) < 0) {
			usbmuxd_log(LL_ERROR, "Failed to submit TX ZLP transfer to device %d-%d: %s", dev->bus, dev->address, libusb_error_name(res));
			libusb_free_transfer(xfer);
			return res;
		}
		collection_add(&dev->tx_xfers, xfer);
	}
	return 0;
}

// Callback from read operation
// Under normal operation this issues a new read transfer request immediately,
// doing a kind of read-callback loop
static void rx_callback(struct libusb_transfer *xfer)
{
	struct usb_device *dev = xfer->user_data;
	usbmuxd_log(LL_SPEW, "RX callback dev %d-%d len %d status %d", dev->bus, dev->address, xfer->actual_length, xfer->status);
	if(xfer->status == LIBUSB_TRANSFER_COMPLETED) {
		device_data_input(dev, xfer->buffer, xfer->actual_length);
		libusb_submit_transfer(xfer);
	} else {
		switch(xfer->status) {
			case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
			case LIBUSB_TRANSFER_ERROR:
				// funny, this happens when we disconnect the device while waiting for a transfer, sometimes
				usbmuxd_log(LL_INFO, "Device %d-%d RX aborted due to error or disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_TIMED_OUT:
				usbmuxd_log(LL_ERROR, "RX transfer timed out for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_CANCELLED:
				usbmuxd_log(LL_DEBUG, "Device %d-%d RX transfer cancelled", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_STALL:
				usbmuxd_log(LL_ERROR, "RX transfer stalled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_NO_DEVICE:
				// other times, this happens, and also even when we abort the transfer after device removal
				usbmuxd_log(LL_INFO, "Device %d-%d RX aborted due to disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_OVERFLOW:
				usbmuxd_log(LL_ERROR, "RX transfer overflow for device %d-%d", dev->bus, dev->address);
				break;
			// and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
			default:
				// this should never be reached.
				break;
		}

		free(xfer->buffer);
		collection_remove(&dev->rx_xfers, xfer);
		libusb_free_transfer(xfer);

		// we can't usb_disconnect here due to a deadlock, so instead mark it as dead and reap it after processing events
		// we'll do device_remove there too
		dev->alive = 0;
	}
}

// Start a read-callback loop for this device
static int start_rx_loop(struct usb_device *dev)
{
	int res;
	void *buf;
	struct libusb_transfer *xfer = libusb_alloc_transfer(0);
	buf = malloc(USB_MRU);
	libusb_fill_bulk_transfer(xfer, dev->handle, dev->ep_in, buf, USB_MRU, rx_callback, dev, 0);
	if((res = libusb_submit_transfer(xfer)) != 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit RX transfer to device %d-%d: %s", dev->bus, dev->address, libusb_error_name(res));
		libusb_free_transfer(xfer);
		return res;
	}

	collection_add(&dev->rx_xfers, xfer);

	return 0;
}

static void get_serial_callback(struct libusb_transfer *transfer)
{
	unsigned int di, si;
	struct usb_device *usbdev = transfer->user_data;

	if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		usbmuxd_log(LL_ERROR, "Failed to request serial for device %d-%d (%i)", usbdev->bus, usbdev->address, transfer->status);
		libusb_free_transfer(transfer);
		return;
	}

	/* De-unicode, taken from libusb */
	unsigned char *data = libusb_control_transfer_get_data(transfer);
	for (di = 0, si = 2; si < data[0] && di < sizeof(usbdev->serial)-1; si += 2) {
		if ((data[si] & 0x80) || (data[si + 1])) /* non-ASCII */
			usbdev->serial[di++] = '?';
		else if (data[si] == '\0')
			break;
		else
			usbdev->serial[di++] = data[si];
	}
	usbdev->serial[di] = '\0';

	usbmuxd_log(LL_INFO, "Got serial '%s' for device %d-%d", usbdev->serial, usbdev->bus, usbdev->address);

	libusb_free_transfer(transfer);

	/* new style UDID: add hyphen between first 8 and following 16 digits */
	if (di == 24) {
		memmove(&usbdev->serial[9], &usbdev->serial[8], 16);
		usbdev->serial[8] = '-';
		usbdev->serial[di+1] = '\0';
	}

	/* Finish setup now */
	if(device_add(usbdev) < 0) {
		usb_disconnect(usbdev);
		return;
	}

	// Spin up NUM_RX_LOOPS parallel usb data retrieval loops
	// Old usbmuxds used only 1 rx loop, but that leaves the
	// USB port sleeping most of the time
	int rx_loops = NUM_RX_LOOPS;
	for (rx_loops = NUM_RX_LOOPS; rx_loops > 0; rx_loops--) {
		if(start_rx_loop(usbdev) < 0) {
			usbmuxd_log(LL_WARNING, "Failed to start RX loop number %d", NUM_RX_LOOPS - rx_loops);
			break;
		}
	}

	// Ensure we have at least 1 RX loop going
	if (rx_loops == NUM_RX_LOOPS) {
		usbmuxd_log(LL_FATAL, "Failed to start any RX loop for device %d-%d",
					usbdev->bus, usbdev->address);
		device_remove(usbdev);
		usb_disconnect(usbdev);
		return;
	} else if (rx_loops > 0) {
		usbmuxd_log(LL_WARNING, "Failed to start all %d RX loops. Going on with %d loops. "
					"This may have negative impact on device read speed.",
					NUM_RX_LOOPS, NUM_RX_LOOPS - rx_loops);
	} else {
		usbmuxd_log(LL_DEBUG, "All %d RX loops started successfully", NUM_RX_LOOPS);
	}
}

static void get_langid_callback(struct libusb_transfer *transfer)
{
	int res;
	struct usb_device *usbdev = transfer->user_data;

	transfer->flags |= LIBUSB_TRANSFER_FREE_BUFFER;

	if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		 usbmuxd_log(LL_ERROR, "Failed to request lang ID for device %d-%d (%i)", usbdev->bus,
				 usbdev->address, transfer->status);
		 libusb_free_transfer(transfer);
		 return;
	}

	unsigned char *data = libusb_control_transfer_get_data(transfer);
	uint16_t langid = (uint16_t)(data[2] | (data[3] << 8));
	usbmuxd_log(LL_INFO, "Got lang ID %u for device %d-%d", langid, usbdev->bus, usbdev->address);

	/* re-use the same transfer */
	libusb_fill_control_setup(transfer->buffer, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
			(uint16_t)((LIBUSB_DT_STRING << 8) | usbdev->devdesc.iSerialNumber),
			langid, 1024 + LIBUSB_CONTROL_SETUP_SIZE);
	libusb_fill_control_transfer(transfer, usbdev->handle, transfer->buffer, get_serial_callback, usbdev, 1000);

	if((res = libusb_submit_transfer(transfer)) < 0) {
		usbmuxd_log(LL_ERROR, "Could not request transfer for device %d-%d: %s", usbdev->bus, usbdev->address, libusb_error_name(res));
		libusb_free_transfer(transfer);
	}
}

static int submit_vendor_specific(struct libusb_device_handle *handle, struct mode_context *context, libusb_transfer_cb_fn callback) 
{
	struct libusb_transfer* ctrl_transfer = libusb_alloc_transfer(0);
	int ret = 0; 
	unsigned char* buffer = calloc(LIBUSB_CONTROL_SETUP_SIZE + context->wLength, 1);
	uint8_t bRequestType = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE;
	libusb_fill_control_setup(buffer, bRequestType, context->bRequest, context->wValue, context->wIndex, context->wLength);
	
	ctrl_transfer->flags = LIBUSB_TRANSFER_FREE_TRANSFER;
	libusb_fill_control_transfer(ctrl_transfer, handle, buffer, callback, context, context->timeout);
	
	ret = libusb_submit_transfer(ctrl_transfer);
	return ret;
}

static struct usb_device* find_device(int bus, int address)
{
	FOREACH(struct usb_device *usbdev, &device_list) {
		if(usbdev->bus == bus && usbdev->address == address) {
			return usbdev;
		}
	} ENDFOREACH
	return NULL;
}

/// @brief guess the current mode
/// @param dev 
/// @param usbdev 
/// @param handle 
/// @return 0 - undetermined, 1 - initial, 2 - valeria, 3 - cdc_ncm, 4 - usbeth+cdc_ncm, 5 - cdc_ncm direct
static int guess_mode(struct libusb_device* dev, struct usb_device *usbdev)
{
	int res, j;
	int has_valeria = 0, has_cdc_ncm = 0, has_usbmux = 0;
	struct libusb_device_descriptor devdesc = usbdev->devdesc;
	struct libusb_config_descriptor *config;
	int bus = usbdev->bus;
	int address = usbdev->address;

	if(devdesc.bNumConfigurations == 1) {
		// CDC-NCM Direct
		return 5;
	}

	if(devdesc.bNumConfigurations <= 4) {
		// Assume this is initial mode
		return 1;
	}

	if(devdesc.bNumConfigurations == 6) {
		// USB Ethernet + CDC-NCM
		return 4;
	}

	if(devdesc.bNumConfigurations != 5) {
		// No known modes with more then 5 configurations
		return 0;
	}

	if((res = libusb_get_config_descriptor_by_value(dev, 5, &config)) != 0) {
		usbmuxd_log(LL_NOTICE, "Could not get configuration 5 descriptor for device %i-%i: %s", bus, address, libusb_error_name(res));
		return 0;
	}

	// Require both usbmux and one of the other interfaces to determine this is a valid configuration
	for(j = 0 ; j < config->bNumInterfaces ; j++) {
		const struct libusb_interface_descriptor *intf = &config->interface[j].altsetting[0];
		if(intf->bInterfaceClass == INTERFACE_CLASS &&
		   intf->bInterfaceSubClass == 42 &&
		   intf->bInterfaceProtocol == 255) {
			has_valeria = 1;
		}
		// https://github.com/torvalds/linux/blob/72a85e2b0a1e1e6fb4ee51ae902730212b2de25c/include/uapi/linux/usb/cdc.h#L22
		// 2 for Communication class, 0xd for CDC NCM subclass
		if(intf->bInterfaceClass == 2 &&
		   intf->bInterfaceSubClass == 0xd) {
			has_cdc_ncm = 1;
		}
		if(intf->bInterfaceClass == INTERFACE_CLASS &&
		   intf->bInterfaceSubClass == INTERFACE_SUBCLASS &&
		   intf->bInterfaceProtocol == INTERFACE_PROTOCOL) {
			has_usbmux = 1;
		}
	}

	libusb_free_config_descriptor(config);

	if(has_valeria && has_usbmux) {
		usbmuxd_log(LL_NOTICE, "Found Valeria and Apple USB Multiplexor in device %i-%i configuration 5", bus, address);
		return 2;
	}

	if(has_cdc_ncm && has_usbmux) {
		usbmuxd_log(LL_NOTICE, "Found CDC-NCM and Apple USB Multiplexor in device %i-%i configuration 5", bus, address);
		return 3;
	}

	return 0;
}

/// @brief Finds and sets the valid configuration, interface and endpoints on the usb_device
static int set_valid_configuration(struct libusb_device* dev, struct usb_device *usbdev, struct libusb_device_handle *handle)
{
	int j, k, res, found = 0;
	struct libusb_config_descriptor *config;
	const struct libusb_interface_descriptor *intf;
	struct libusb_device_descriptor devdesc = usbdev->devdesc;
	int bus = usbdev->bus;
	int address = usbdev->address;
	int current_config = 0;

	if((res = libusb_get_configuration(handle, &current_config)) != 0) {
		usbmuxd_log(LL_WARNING, "Could not get current configuration for device %d-%d: %s", bus, address, libusb_error_name(res));
		return -1;
	}

	for(j = devdesc.bNumConfigurations ; j > 0  ; j--) {
		if((res = libusb_get_config_descriptor_by_value(dev, j, &config)) != 0) {
			usbmuxd_log(LL_NOTICE, "Could not get configuration %i descriptor for device %i-%i: %s", j, bus, address, libusb_error_name(res));
			continue;
		}
		for(k = 0 ; k < config->bNumInterfaces ; k++) {
			intf = &config->interface[k].altsetting[0];
			if(intf->bInterfaceClass == INTERFACE_CLASS || 
			   intf->bInterfaceSubClass == INTERFACE_SUBCLASS || 
			   intf->bInterfaceProtocol == INTERFACE_PROTOCOL) {
				usbmuxd_log(LL_NOTICE, "Found usbmux interface for device %i-%i: %i", bus, address, intf->bInterfaceNumber);
				if(intf->bNumEndpoints != 2) {
					usbmuxd_log(LL_WARNING, "Endpoint count mismatch for interface %i of device %i-%i", intf->bInterfaceNumber, bus, address);
					continue;
				}
				if((intf->endpoint[0].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT &&
				   (intf->endpoint[1].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_IN) {
					usbdev->interface = intf->bInterfaceNumber;
					usbdev->ep_out = intf->endpoint[0].bEndpointAddress;
					usbdev->ep_in = intf->endpoint[1].bEndpointAddress;
					usbmuxd_log(LL_INFO, "Found interface %i with endpoints %02x/%02x for device %i-%i", usbdev->interface, usbdev->ep_out, usbdev->ep_in, bus, address);
					found = 1;
					break;
				} else if((intf->endpoint[1].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT &&
						  (intf->endpoint[0].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_IN) {
					usbdev->interface = intf->bInterfaceNumber;
					usbdev->ep_out = intf->endpoint[1].bEndpointAddress;
					usbdev->ep_in = intf->endpoint[0].bEndpointAddress;
					usbmuxd_log(LL_INFO, "Found interface %i with swapped endpoints %02x/%02x for device %i-%i", usbdev->interface, usbdev->ep_out, usbdev->ep_in, bus, address);
					found = 1;
					break;
				} else {
					usbmuxd_log(LL_WARNING, "Endpoint type mismatch for interface %i of device %i-%i", intf->bInterfaceNumber, bus, address);
				}
			}
		}
		if(!found) {
			libusb_free_config_descriptor(config);
			continue;
		}
		// If set configuration is required, try to first detach all kernel drivers
		if (current_config == 0) {
			usbmuxd_log(LL_DEBUG, "Device %d-%d is unconfigured", bus, address);
		}
		if(current_config == 0 || config->bConfigurationValue != current_config) {
			usbmuxd_log(LL_NOTICE, "Changing configuration of device %i-%i: %i -> %i", bus, address, current_config, config->bConfigurationValue);
			for(k=0 ; k < config->bNumInterfaces ; k++) {
				const struct libusb_interface_descriptor *intf1 = &config->interface[k].altsetting[0];
				if((res = libusb_kernel_driver_active(handle, intf1->bInterfaceNumber)) < 0) {
					usbmuxd_log(LL_NOTICE, "Could not check kernel ownership of interface %d for device %d-%d: %s", intf1->bInterfaceNumber, bus, address, libusb_error_name(res));
					continue;
				}
				if(res == 1) {
					usbmuxd_log(LL_INFO, "Detaching kernel driver for device %d-%d, interface %d", bus, address, intf1->bInterfaceNumber);
					if((res = libusb_detach_kernel_driver(handle, intf1->bInterfaceNumber)) < 0) {
						usbmuxd_log(LL_WARNING, "Could not detach kernel driver, configuration change will probably fail! %s", libusb_error_name(res));
						continue;
					}
				}
			}
			if((res = libusb_set_configuration(handle, j)) != 0) {
				usbmuxd_log(LL_WARNING, "Could not set configuration %d for device %d-%d: %s", j, bus, address, libusb_error_name(res));
				libusb_free_config_descriptor(config);
				continue;
			}
		}
		
		libusb_free_config_descriptor(config);
		break;
	}

	if(!found) {
		usbmuxd_log(LL_WARNING, "Could not find a suitable USB interface for device %i-%i", bus, address);
		return -1;
	}

	return 0;
}

static void device_complete_initialization(struct mode_context *context, struct libusb_device_handle *handle) 
{
	struct usb_device *usbdev = find_device(context->bus, context->address);
	if(!usbdev) {
		usbmuxd_log(LL_ERROR, "Device %d-%d is missing from device list, aborting initialization", context->bus, context->address);
		return;
	}
	struct libusb_device *dev = context->dev;
	struct libusb_device_descriptor devdesc = usbdev->devdesc;
	int bus = context->bus;
	int address = context->address;
	int res;
	struct libusb_transfer *transfer;

	if((res = set_valid_configuration(dev, usbdev, handle)) != 0) {
		usbdev->alive = 0;
		return;
	}

	if((res = libusb_claim_interface(handle, usbdev->interface)) != 0) {
		usbmuxd_log(LL_WARNING, "Could not claim interface %d for device %d-%d: %s", usbdev->interface, bus, address, libusb_error_name(res));
		usbdev->alive = 0;
		return;
	}

	transfer = libusb_alloc_transfer(0);
	if(!transfer) {
		usbmuxd_log(LL_WARNING, "Failed to allocate transfer for device %d-%d: %s", bus, address, libusb_error_name(res));
		usbdev->alive = 0;
		return;
	}

	unsigned char *transfer_buffer = malloc(1024 + LIBUSB_CONTROL_SETUP_SIZE + 8);
	if (!transfer_buffer) {
		usbmuxd_log(LL_WARNING, "Failed to allocate transfer buffer for device %d-%d: %s", bus, address, libusb_error_name(res));
		usbdev->alive = 0;
		return;
	}
	memset(transfer_buffer, '\0', 1024 + LIBUSB_CONTROL_SETUP_SIZE + 8);

	usbdev->serial[0] = 0;
	usbdev->bus = bus;
	usbdev->address = address;
	usbdev->devdesc = devdesc;
	usbdev->speed = 480000000;
	usbdev->handle = handle;
	usbdev->alive = 1;
	usbdev->wMaxPacketSize = libusb_get_max_packet_size(dev, usbdev->ep_out);
	if (usbdev->wMaxPacketSize <= 0) {
		usbmuxd_log(LL_ERROR, "Could not determine wMaxPacketSize for device %d-%d, setting to 64", usbdev->bus, usbdev->address);
		usbdev->wMaxPacketSize = 64;
	} else {
		usbmuxd_log(LL_INFO, "Using wMaxPacketSize=%d for device %d-%d", usbdev->wMaxPacketSize, usbdev->bus, usbdev->address);
	}

	switch (libusb_get_device_speed(dev)) {
		case LIBUSB_SPEED_LOW:
			usbdev->speed = 1500000;
			break;
		case LIBUSB_SPEED_FULL:
			usbdev->speed = 12000000;
			break;
		case LIBUSB_SPEED_SUPER:
			usbdev->speed = 5000000000;
			break;
		case LIBUSB_SPEED_SUPER_PLUS:
			usbdev->speed = 10000000000;
			break;
		case LIBUSB_SPEED_HIGH:
		case LIBUSB_SPEED_UNKNOWN:
		default:
			usbdev->speed = 480000000;
			break;
	}

	usbmuxd_log(LL_INFO, "USB Speed is %g MBit/s for device %d-%d", (double)(usbdev->speed / 1000000.0), usbdev->bus, usbdev->address);

	/**
	 * From libusb:
	 * 	Asking for the zero'th index is special - it returns a string
	 * 	descriptor that contains all the language IDs supported by the
	 * 	device.
	 **/
	libusb_fill_control_setup(transfer_buffer, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_STRING << 8, 0, 1024 + LIBUSB_CONTROL_SETUP_SIZE);
	libusb_fill_control_transfer(transfer, handle, transfer_buffer, get_langid_callback, usbdev, 1000);

	if((res = libusb_submit_transfer(transfer)) < 0) {
		usbmuxd_log(LL_ERROR, "Could not request transfer for device %d-%d: %s", usbdev->bus, usbdev->address, libusb_error_name(res));
		libusb_free_transfer(transfer);
		free(transfer_buffer);
		usbdev->alive = 0;
		return;	
	}
}

static void switch_mode_cb(struct libusb_transfer* transfer) 
{
	// For old devices not supporting mode swtich, if anything goes wrong - continue in current mode
	struct mode_context* context = transfer->user_data;
	struct usb_device *dev = find_device(context->bus, context->address);
	if(!dev) {
		usbmuxd_log(LL_WARNING, "Device %d-%d is missing from device list", context->bus, context->address);
	}
	if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		usbmuxd_log(LL_ERROR, "Failed to request mode switch for device %i-%i (%i). Completing initialization in current mode", 
			context->bus, context->address, transfer->status);
		device_complete_initialization(context, transfer->dev_handle);
	}
	else {
		unsigned char *data = libusb_control_transfer_get_data(transfer);
		if(data[0] != 0) {
			usbmuxd_log(LL_INFO, "Received unexpected response for device %i-%i mode switch (%i). Completing initialization in current mode", 
				context->bus, context->address, data[0]);
			device_complete_initialization(context, transfer->dev_handle);
		}
	}
	free(context);
	if(transfer->buffer)
		free(transfer->buffer);
}

static void get_mode_cb(struct libusb_transfer* transfer) 
{
	// For old devices not supporting mode swtich, if anything goes wrong - continue in current mode
	int res;
	struct mode_context* context = transfer->user_data;
	struct usb_device *dev = find_device(context->bus, context->address);
	if(!dev) {
		usbmuxd_log(LL_ERROR, "Device %d-%d is missing from device list, aborting mode switch", context->bus, context->address);
		free(context);
		return;
	}

	if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		usbmuxd_log(LL_ERROR, "Failed to request get mode for device %i-%i (%i). Completing initialization in current mode", 
			context->bus, context->address, transfer->status);
		device_complete_initialization(context, transfer->dev_handle);
		free(context);
		return;
	}

	unsigned char *data = libusb_control_transfer_get_data(transfer);

	char* desired_mode_char = getenv(ENV_DEVICE_MODE);
	int desired_mode = desired_mode_char ? atoi(desired_mode_char) : 1;
	int guessed_mode = guess_mode(context->dev, dev);

	// Response is 3:3:3:0 for initial mode, 5:3:3:0 otherwise.
	usbmuxd_log(LL_INFO, "Received response %i:%i:%i:%i for get_mode request for device %i-%i", data[0], data[1], data[2], data[3], context->bus, context->address);
	if(desired_mode >= 1 && desired_mode <= 5 &&
	   guessed_mode > 0 && // do not switch mode if guess failed
	   guessed_mode != desired_mode) {
		usbmuxd_log(LL_WARNING, "Switching device %i-%i mode to %i", context->bus, context->address, desired_mode);
		
		context->bRequest = APPLE_VEND_SPECIFIC_SET_MODE;
		context->wValue = 0;
		context->wIndex = desired_mode;
		context->wLength = 1;

		if((res = submit_vendor_specific(transfer->dev_handle, context, switch_mode_cb)) != 0) {
			usbmuxd_log(LL_WARNING, "Could not request to switch mode %i for device %i-%i (%i)", context->wIndex, context->bus, context->address, res);
			dev->alive = 0;
			free(context);
		}
	} 
	else {
		usbmuxd_log(LL_WARNING, "Skipping switch device %i-%i mode from %i to %i", context->bus, context->address, guessed_mode, desired_mode);
		device_complete_initialization(context, transfer->dev_handle);
		free(context);
	}
	if(transfer->buffer)
		free(transfer->buffer);
}

static int usb_device_add(libusb_device* dev)
{
	int res;
	// the following are non-blocking operations on the device list
	uint8_t bus = libusb_get_bus_number(dev);
	uint8_t address = libusb_get_device_address(dev);
	struct libusb_device_descriptor devdesc;
	struct usb_device *usbdev = find_device(bus, address);
	if(usbdev) {
		usbdev->alive = 1;
		return 0; //device already found
	}

	if((res = libusb_get_device_descriptor(dev, &devdesc)) != 0) {
		usbmuxd_log(LL_WARNING, "Could not get device descriptor for device %d-%d: %s", bus, address, libusb_error_name(res));
		return -1;
	}
	if(devdesc.idVendor != VID_APPLE)
		return -1;
	if((devdesc.idProduct != PID_APPLE_T2_COPROCESSOR) &&
	   ((devdesc.idProduct < PID_APPLE_SILICON_RESTORE_LOW) ||
		(devdesc.idProduct > PID_APPLE_SILICON_RESTORE_MAX)) &&
	   ((devdesc.idProduct < PID_RANGE_LOW) ||
		(devdesc.idProduct > PID_RANGE_MAX)))
		return -1;
	libusb_device_handle *handle;
	usbmuxd_log(LL_INFO, "Found new device with v/p %04x:%04x at %d-%d", devdesc.idVendor, devdesc.idProduct, bus, address);
	// No blocking operation can follow: it may be run in the libusb hotplug callback and libusb will refuse any
	// blocking call
	if((res = libusb_open(dev, &handle)) != 0) {
		usbmuxd_log(LL_WARNING, "Could not open device %d-%d: %s", bus, address, libusb_error_name(res));
		return -1;
	}

	// Add the created handle to the device list, so we can close it in case of failure/disconnection
	usbdev = malloc(sizeof(struct usb_device));
	memset(usbdev, 0, sizeof(*usbdev));

	usbdev->serial[0] = 0;
	usbdev->bus = bus;
	usbdev->address = address;
	usbdev->devdesc = devdesc;
	usbdev->speed = 0;
	usbdev->handle = handle;
	usbdev->alive = 1;

	collection_init(&usbdev->tx_xfers);
	collection_init(&usbdev->rx_xfers);

	collection_add(&device_list, usbdev);

	// On top of configurations, Apple have multiple "modes" for devices, namely:
	// 1: An "initial" mode with 4 configurations
	// 2: "Valeria" mode, where configuration 5 is included with interface for H.265 video capture (activated when recording screen with QuickTime in macOS)
	// 3: "CDC NCM" mode, where configuration 5 is included with interface for Ethernet/USB (activated using internet-sharing feature in macOS)
	// Request current mode asynchroniously, so it can be changed in callback if needed
	usbmuxd_log(LL_INFO, "Requesting current mode from device %i-%i", bus, address);
	struct mode_context* context = malloc(sizeof(struct mode_context));
	context->dev = dev;
	context->bus = bus;
	context->address = address;
	context->bRequest = APPLE_VEND_SPECIFIC_GET_MODE;
	context->wValue = 0;
	context->wIndex = 0;
	context->wLength = 4;
	context->timeout = 1000;

	if(submit_vendor_specific(handle, context, get_mode_cb) != 0) {
		usbmuxd_log(LL_WARNING, "Could not request current mode from device %d-%d", bus, address);
		// Schedule device for close and cleanup
		usbdev->alive = 0;
		return -1;
	}
	return 0;
}

int usb_discover(void)
{
	int cnt, i;
	int valid_count = 0;
	libusb_device **devs;

	cnt = libusb_get_device_list(NULL, &devs);
	if(cnt < 0) {
		usbmuxd_log(LL_WARNING, "Could not get device list: %d", cnt);
		devlist_failures++;
		// sometimes libusb fails getting the device list if you've just removed something
		if(devlist_failures > 5) {
			usbmuxd_log(LL_FATAL, "Too many errors getting device list");
			return cnt;
		} else {
			get_tick_count(&next_dev_poll_time);
			next_dev_poll_time.tv_usec += DEVICE_POLL_TIME * 1000;
			next_dev_poll_time.tv_sec += next_dev_poll_time.tv_usec / 1000000;
			next_dev_poll_time.tv_usec = next_dev_poll_time.tv_usec % 1000000;
			return 0;
		}
	}
	devlist_failures = 0;

	usbmuxd_log(LL_SPEW, "usb_discover: scanning %d devices", cnt);

	// Mark all devices as dead, and do a mark-sweep like
	// collection of dead devices
	FOREACH(struct usb_device *usbdev, &device_list) {
		usbdev->alive = 0;
	} ENDFOREACH

	// Enumerate all USB devices and mark the ones we already know
	// about as live, again
	for(i=0; i<cnt; i++) {
		libusb_device *dev = devs[i];
		if (usb_device_add(dev) < 0) {
			continue;
		}
		valid_count++;
	}

	// Clean out any device we didn't mark back as live
	reap_dead_devices();

	libusb_free_device_list(devs, 1);

	get_tick_count(&next_dev_poll_time);
	next_dev_poll_time.tv_usec += DEVICE_POLL_TIME * 1000;
	next_dev_poll_time.tv_sec += next_dev_poll_time.tv_usec / 1000000;
	next_dev_poll_time.tv_usec = next_dev_poll_time.tv_usec % 1000000;

	return valid_count;
}

const char *usb_get_serial(struct usb_device *dev)
{
	if(!dev->handle) {
		return NULL;
	}
	return dev->serial;
}

uint32_t usb_get_location(struct usb_device *dev)
{
	if(!dev->handle) {
		return 0;
	}
	return (dev->bus << 16) | dev->address;
}

uint16_t usb_get_pid(struct usb_device *dev)
{
	if(!dev->handle) {
		return 0;
	}
	return dev->devdesc.idProduct;
}

uint64_t usb_get_speed(struct usb_device *dev)
{
	if (!dev->handle) {
		return 0;
	}
	return dev->speed;
}

void usb_get_fds(struct fdlist *list)
{
	const struct libusb_pollfd **usbfds;
	const struct libusb_pollfd **p;
	usbfds = libusb_get_pollfds(NULL);
	if(!usbfds) {
		usbmuxd_log(LL_ERROR, "libusb_get_pollfds failed");
		return;
	}
	p = usbfds;
	while(*p) {
		fdlist_add(list, FD_USB, (*p)->fd, (*p)->events);
		p++;
	}
	free(usbfds);
}

void usb_autodiscover(int enable)
{
	usbmuxd_log(LL_DEBUG, "usb polling enable: %d", enable);
	device_polling = enable;
	device_hotplug = enable;
}

static int dev_poll_remain_ms(void)
{
	int msecs;
	struct timeval tv;
	if(!device_polling)
		return 100000; // devices will never be polled if this is > 0
	get_tick_count(&tv);
	msecs = (next_dev_poll_time.tv_sec - tv.tv_sec) * 1000;
	msecs += (next_dev_poll_time.tv_usec - tv.tv_usec) / 1000;
	if(msecs < 0)
		return 0;
	return msecs;
}

int usb_get_timeout(void)
{
	struct timeval tv;
	int msec;
	int res;
	int pollrem;
	pollrem = dev_poll_remain_ms();
	res = libusb_get_next_timeout(NULL, &tv);
	if(res == 0)
		return pollrem;
	if(res < 0) {
		usbmuxd_log(LL_ERROR, "libusb_get_next_timeout failed: %s", libusb_error_name(res));
		return pollrem;
	}
	msec = tv.tv_sec * 1000;
	msec += tv.tv_usec / 1000;
	if(msec > pollrem)
		return pollrem;
	return msec;
}

int usb_process(void)
{
	int res;
	struct timeval tv;
	tv.tv_sec = tv.tv_usec = 0;
	res = libusb_handle_events_timeout(NULL, &tv);
	if(res < 0) {
		usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout failed: %s", libusb_error_name(res));
		return res;
	}

	// reap devices marked dead due to an RX error
	reap_dead_devices();

	if(dev_poll_remain_ms() <= 0) {
		res = usb_discover();
		if(res < 0) {
			usbmuxd_log(LL_ERROR, "usb_discover failed: %s", libusb_error_name(res));
			return res;
		}
	}
	return 0;
}

int usb_process_timeout(int msec)
{
	int res;
	struct timeval tleft, tcur, tfin;
	get_tick_count(&tcur);
	tfin.tv_sec = tcur.tv_sec + (msec / 1000);
	tfin.tv_usec = tcur.tv_usec + (msec % 1000) * 1000;
	tfin.tv_sec += tfin.tv_usec / 1000000;
	tfin.tv_usec %= 1000000;
	while((tfin.tv_sec > tcur.tv_sec) || ((tfin.tv_sec == tcur.tv_sec) && (tfin.tv_usec > tcur.tv_usec))) {
		tleft.tv_sec = tfin.tv_sec - tcur.tv_sec;
		tleft.tv_usec = tfin.tv_usec - tcur.tv_usec;
		if(tleft.tv_usec < 0) {
			tleft.tv_usec += 1000000;
			tleft.tv_sec -= 1;
		}
		res = libusb_handle_events_timeout(NULL, &tleft);
		if(res < 0) {
			usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout failed: %s", libusb_error_name(res));
			return res;
		}
		// reap devices marked dead due to an RX error
		reap_dead_devices();
		get_tick_count(&tcur);
	}
	return 0;
}

#ifdef HAVE_LIBUSB_HOTPLUG_API
static libusb_hotplug_callback_handle usb_hotplug_cb_handle;

static int usb_hotplug_cb(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data)
{
	if (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED == event) {
		if (device_hotplug) {
			usb_device_add(device);
		}
	} else if (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT == event) {
		uint8_t bus = libusb_get_bus_number(device);
		uint8_t address = libusb_get_device_address(device);
		FOREACH(struct usb_device *usbdev, &device_list) {
			if(usbdev->bus == bus && usbdev->address == address) {
				usbdev->alive = 0;
				device_remove(usbdev);
				break;
			}
		} ENDFOREACH
	} else {
		usbmuxd_log(LL_ERROR, "Unhandled event %d", event);
	}
	return 0;
}
#endif

int usb_init(void)
{
	int res;
	const struct libusb_version* libusb_version_info = libusb_get_version();
	usbmuxd_log(LL_NOTICE, "Using libusb %d.%d.%d", libusb_version_info->major, libusb_version_info->minor, libusb_version_info->micro);

	devlist_failures = 0;
	device_polling = 1;
	res = libusb_init(NULL);

	if (res != 0) {
		usbmuxd_log(LL_FATAL, "libusb_init failed: %s", libusb_error_name(res));
		return -1;
	}

#if LIBUSB_API_VERSION >= 0x01000106
	libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, (log_level >= LL_DEBUG ? LIBUSB_LOG_LEVEL_DEBUG: (log_level >= LL_WARNING ? LIBUSB_LOG_LEVEL_WARNING: LIBUSB_LOG_LEVEL_NONE)));
#else
	libusb_set_debug(NULL, (log_level >= LL_DEBUG ? LIBUSB_LOG_LEVEL_DEBUG: (log_level >= LL_WARNING ? LIBUSB_LOG_LEVEL_WARNING: LIBUSB_LOG_LEVEL_NONE)));
#endif

	collection_init(&device_list);

#ifdef HAVE_LIBUSB_HOTPLUG_API
	if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
		usbmuxd_log(LL_INFO, "Registering for libusb hotplug events");
		res = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, LIBUSB_HOTPLUG_ENUMERATE, VID_APPLE, LIBUSB_HOTPLUG_MATCH_ANY, 0, usb_hotplug_cb, NULL, &usb_hotplug_cb_handle);
		if (res == LIBUSB_SUCCESS) {
			device_polling = 0;
		} else {
			usbmuxd_log(LL_ERROR, "ERROR: Could not register for libusb hotplug events. %s", libusb_error_name(res));
		}
	} else {
		usbmuxd_log(LL_ERROR, "libusb does not support hotplug events");
	}
#endif
	if (device_polling) {
		res = usb_discover();
		if (res >= 0) {
		}
	} else {
		res = collection_count(&device_list);
	}
	return res;
}

void usb_shutdown(void)
{
	usbmuxd_log(LL_DEBUG, "usb_shutdown");

#ifdef HAVE_LIBUSB_HOTPLUG_API
	libusb_hotplug_deregister_callback(NULL, usb_hotplug_cb_handle);
#endif

	FOREACH(struct usb_device *usbdev, &device_list) {
		device_remove(usbdev);
		usb_disconnect(usbdev);
	} ENDFOREACH
	collection_free(&device_list);
	libusb_exit(NULL);
}
