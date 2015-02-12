#ifndef USB_MONITOR_H
#define USB_MONITOR_H

#include <stdio.h>
#include <stdint.h>
#include <sys/queue.h>
#include <libusb-1.0/libusb.h>

struct usb_port;
struct backend_event_loop;
struct backend_epoll_handle;

#define DEFAULT_TIMEOUT_SEC 5
#define ADDED_TIMEOUT_SEC 10
#define USB_RETRANS_LIMIT 5
#define PING_OUTPUT 20 //Only write ping sucess ~ever 100 sec
#define USB_PATH_MAX 8 //len(path) + bus number

//port function pointers
typedef void (*print_port)(struct usb_port *port);
typedef void (*update_port)(struct usb_port *port);
typedef void (*handle_timeout)(struct usb_port *port);

//The device pointed to here is the device that will be used for comparison when
//new hubs are added
#define USB_HUB_MANDATORY \
    libusb_device *hub_dev; \
    LIST_ENTRY(usb_hub) hub_next; \
    uint8_t num_ports

//Size of path is 8 since it is bus + max depth (7)
//parent might be NULL
#define USB_PORT_MANDATORY \
    struct usb_hub *parent; \
    struct usb_monitor_ctx *ctx; \
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
    uint8_t ping_cnt; \
    uint8_t port_num; \
    uint8_t ping_buf[LIBUSB_CONTROL_SETUP_SIZE + 2]; \
    uint8_t path[USB_PATH_MAX]; \
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
    struct backend_event_loop *event_loop;
    struct backend_epoll_handle *libusb_handle;
    struct timeval last_restart;
    struct timeval last_dev_check;
    FILE* logfile;
    LIST_HEAD(hubs, usb_hub) hub_list;
    LIST_HEAD(ports, usb_port) port_list;
    struct ports timeout_list;
};

//We fake a callback in the device iteration function
int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data);

#endif
