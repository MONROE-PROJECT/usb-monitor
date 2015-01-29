#ifndef USB_MONITOR_H
#define USB_MONITOR_H

#include <stdint.h>
#include <sys/queue.h>
#include <libusb-1.0/libusb.h>

struct usb_port;

#define DEFAULT_TIMEOUT_SEC 5
#define USB_RETRANS_LIMIT 5

//port function pointers
typedef void (*print_port)(struct usb_port *port);
typedef void (*update_port)(struct usb_port *port);
typedef void (*handle_timeout)(struct usb_port *port);

//The device pointed to here is the device that will be used for comparison when
//new hubs are added
#define USB_HUB_MANDATORY \
    libusb_device *hub_dev; \
    struct usb_monitor_ctx *ctx; \
    LIST_ENTRY(usb_hub) hub_next

//Size of path is 8 since it is bus + max depth (7)
#define USB_PORT_MANDATORY \
    struct usb_hub *parent; \
    libusb_device *dev; \
    libusb_device_handle *dev_handle;\
    print_port output; \
    update_port update; \
    handle_timeout timeout; \
    uint64_t timeout_expire; \
    uint16_t vid; \
    uint16_t pid; \
    uint8_t status; \
    uint8_t pwr_state; \
    uint8_t msg_mode; \
    uint8_t path_len; \
    uint8_t num_retrans; \
    uint8_t ping_buf[LIBUSB_CONTROL_SETUP_SIZE + 2]; \
    uint8_t path[8]; \
    LIST_ENTRY(usb_port) port_next; \
    LIST_ENTRY(usb_port) timeout_next

enum port_msg {
    IDLE = 0,
    PING,
    RESET
};

enum port_status {
    PORT_NO_DEV_CONNECTED = 0,
    PORT_DEV_CONNECTED,
};

//We assume port is always on. This is not neccessarily correct, but the YKUSH
//does not export the power state of a port. If we are incorrect, problem will
//be solved by the part of the code which restarts a port if no device is
//connected
enum power_state {
    POWER_OFF = 0,
    POWER_ON,
};

struct usb_hub {
    USB_HUB_MANDATORY;
};

struct usb_port {
    USB_PORT_MANDATORY;
};

struct usb_monitor_ctx {
    LIST_HEAD(hubs, usb_hub) hub_list;
    LIST_HEAD(ports, usb_port) port_list;
    struct ports timeout_list;
};

//Callback used when devices are added. Also, called from _lists whenever a hub
//is added, since we need to check if already seen devices belong to hub
void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev);
#endif
