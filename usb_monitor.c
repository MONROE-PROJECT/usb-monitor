#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <json-c/json.h>
#include <libusb-1.0/libusb.h>

#include "usb_monitor.h"
#include "ykush_handler.h"
#include "usb_monitor_lists.h"
#include "usb_helpers.h"
#include "gpio_handler.h"
#include "usb_logging.h"

//Kept global so that I can access it from the signal handler
static struct usb_monitor_ctx *usbmon_ctx = NULL;

static void usb_monitor_print_ports(struct usb_monitor_ctx *ctx)
{
    struct usb_port *itr;

    LIST_FOREACH(itr, &(ctx->port_list), port_next)
        itr->output(itr);

    fprintf(ctx->logfile, "\n");
}

static void usb_monitor_reset_all_ports(struct usb_monitor_ctx *ctx, uint8_t forced)
{
    struct usb_port *itr;

    LIST_FOREACH(itr, &(ctx->port_list), port_next) {
        //Only restart which are not connected and are currently not being reset
        if (forced ||
            (itr->status == PORT_NO_DEV_CONNECTED &&
            itr->msg_mode != RESET))
            itr->update(itr);
    }
}

/* libusb-callbacks for when devices are added/removed. It is also called
 * manually when we detect a hub, since we risk devices being added before we
 * see for example the YKUSH HID device */
void usb_device_added(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    //Check if device is connected to a port we control
    struct usb_port *port;
    struct libusb_device_descriptor desc;
    uint8_t path[USB_PATH_MAX];
    uint8_t path_len;

    libusb_get_device_descriptor(dev, &desc);

    //This is duplicated code from the event callback. However, it is currently
    //needed in order to handle the case were a hub fails to be added (for
    //example if we can't claim device). When this happens, we will iterate
    //through device and call usb_device_added for each. Hubs can be part of
    //this list, thus, this check is needed here as well
    //TODO: Think about how to unify code
    if (desc.idVendor == YKUSH_VID && desc.idProduct == YKUSH_PID) {
        ykush_event_cb(NULL, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, ctx);
        return;
    }
    
    usb_helpers_fill_port_array(dev, path, &path_len);
    port = usb_monitor_lists_find_port_path(ctx, path, path_len);

    if (!port)
        return;

    //Need to check port if it already has a device, since we can risk that we
    //are called two times for one device
    if (port->dev && port->dev == dev)
        return;

    USB_DEBUG_PRINT(ctx->logfile, "Device: %.4x:%.4x added\n", desc.idVendor, desc.idProduct);

    //We need to configure port. So far, this is all generic
    port->vid = desc.idVendor;
    port->pid = desc.idProduct;
    port->status = PORT_DEV_CONNECTED;
    port->dev = dev;
    port->msg_mode = PING;
    libusb_ref_device(dev);

    usb_monitor_print_ports(ctx);

    //Whenever we detect a device, we need to add to timeout to send ping.
    //However, we need to wait longer than the initial five seconds to let
    //usb_modeswitch potentially works its magic
    //TODO: Do not use magic value
    usb_helpers_start_timeout(port, DEFAULT_TIMEOUT_SEC);
}

static void usb_device_removed(struct usb_monitor_ctx *ctx, libusb_device *dev)
{
    uint8_t path[USB_PATH_MAX];
    uint8_t path_len;
    struct usb_port *port = NULL;

    usb_helpers_fill_port_array(dev, path, &path_len);
    port = usb_monitor_lists_find_port_path(ctx, path, path_len);

    if (!port)
        return;

    usb_helpers_reset_port(port);
    usb_monitor_print_ports(ctx);
}

//Generic device callback
static int usb_monitor_cb(libusb_context *ctx, libusb_device *device,
                          libusb_hotplug_event event, void *user_data)
{
    struct usb_monitor_ctx *usbmon_ctx = user_data;
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(device, &desc);

    //Multiple callbacks can be called multiple times, so it makes little sense
    //to register a separate ykush callback, when we anyway have to filter here
    if (desc.idVendor == YKUSH_VID && desc.idProduct == YKUSH_PID) {
        ykush_event_cb(ctx, device, event, user_data);
        return 0;
    }

    //So far, we assume that all hubs will have separate callbacks, so ignore
    //those
    //TODO: Add support for hubs in hubs?
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB)
        return 0;

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        usb_device_added(usbmon_ctx, device);
    else
        usb_device_removed(usbmon_ctx, device);


    return 0;
}

static void usb_monitor_check_timeouts(struct usb_monitor_ctx *ctx)
{
    struct usb_port *timeout_itr = NULL, *old_timeout = NULL;
    struct timeval tv;
    uint64_t cur_time;

    gettimeofday(&tv, NULL);
    cur_time = (tv.tv_sec * 1e6) + tv.tv_usec;

    timeout_itr = ctx->timeout_list.lh_first;

    while (timeout_itr != NULL) {
        if (cur_time >= timeout_itr->timeout_expire) {
            //Detatch from list, then run timeout
            old_timeout = timeout_itr;
            timeout_itr = timeout_itr->timeout_next.le_next;

            usb_monitor_lists_del_timeout(old_timeout);
            old_timeout->timeout(old_timeout);
        } else {
            timeout_itr = timeout_itr->timeout_next.le_next;
        }
    }
}

static uint8_t usb_monitor_parse_handlers(struct usb_monitor_ctx *ctx,
                                          struct json_object *handlers)
{
    int handlers_len = 0, i;
    uint8_t unknown_elem = 0;
    const char *handler_name = NULL;
    struct json_object *arr_obj, *handler_obj = NULL;
    
    handlers_len = json_object_array_length(handlers);

    for (i = 0; i < handlers_len; i++) {
        handler_name = NULL;
        handler_obj = NULL;

        arr_obj = json_object_array_get_idx(handlers, i);

        json_object_object_foreach(arr_obj, key, val) {
            if (!strcmp(key, "name")) {
                handler_name = json_object_get_string(val);
                continue;
            } else if(!strcmp(key, "ports")) {
                handler_obj = val;
                continue;
            } else {
                unknown_elem = 1;
                break;
            }
        }

        if (handler_name == NULL || handler_obj == NULL || unknown_elem) {
            fprintf(stderr, "Incorrect handler object found in JSON\n");
            return 1;
        }

        if (!strcmp("GPIO", handler_name)) {
            if (gpio_handler_parse_json(ctx, handler_obj))
                return 1;
        } else {
            fprintf(stderr, "Unknown handler in JSON\n");
            return 1;
        }
    }

    return 0;
}

//Return 0 on success, 1 on failure
static uint8_t usb_monitor_parse_config(struct usb_monitor_ctx *ctx,
                                        const char *config_file_name)
{
    //Limit the number of bytes we read from file
    char buf[1024];
    FILE *conf_file;
    //TODO: Clean up a bit here
    struct json_object *conf_json, *top_value;
    struct lh_entry *obj_table;
    int retval = 0;
    const char *obj_name;

    memset(buf, 0, sizeof(buf));
    conf_file = fopen(config_file_name, "re");

    if (conf_file == NULL) {
        fprintf(stderr, "Failed to open config file\n");
        return 1;
    }

    retval = fread(buf, 1, 1024, conf_file);

    if (retval != sizeof(buf) &&
        ferror(conf_file)) {
        fprintf(stderr, "Failed to read from config file\n");
        fclose(conf_file);
        return 1;
    } else {
        fclose(conf_file);
    }

    //Parse JSON
    conf_json = json_tokener_parse(buf);

    if (conf_json == NULL) {
        fprintf(stderr, "Failed to parse JSON\n");
        return 1;
    }

    obj_table = json_object_get_object(conf_json)->head;
    obj_name = (char*) obj_table->k;

    if (strcmp("handlers", obj_name)) {
        fprintf(stderr, "Found unknown top-level object in JSON\n");
        json_object_put(conf_json);
        return 1;
    }

    //Iterate through the handlers
    top_value = (struct json_object *) obj_table->v;

    if (json_object_get_type(top_value) != json_type_array) {
        fprintf(stderr, "Incorrect value for top-leve value\n");
        json_object_put(conf_json);
        return 1;
    }

    retval = usb_monitor_parse_handlers(ctx, top_value);

    json_object_put(conf_json);
    
    return retval;
}

static void usb_monitor_signal_handler(int signum)
{
    USB_DEBUG_PRINT(usbmon_ctx->logfile, "Signalled to restart all ports\n");
    usb_monitor_reset_all_ports(usbmon_ctx, 1);
}

int main(int argc, char *argv[])
{
    int retval = 0;
    struct timeval tv = {1,0};
    struct timeval last_restart, last_dev_check, cur_time;
    uint8_t daemonize = 0;
    char *conf_file_name = NULL;
    struct sigaction sig_handler;
    int32_t pid_fd;

    //We should only allow one running instance of usb_monitor
    pid_fd = open("/var/run/usb_monitor.pid", O_CREAT | O_RDWR | O_CLOEXEC, 0644);

    if (pid_fd == -1 ||
        lockf(pid_fd, F_TLOCK, 0)) {
        exit(EXIT_FAILURE);
    }

    memset(&sig_handler, 0, sizeof(sig_handler));

    usbmon_ctx = malloc(sizeof(struct usb_monitor_ctx));

    if (usbmon_ctx == NULL) {
        fprintf(stderr, "Failed to allocated application context struct\n");
        exit(EXIT_FAILURE);
    }
    
    usbmon_ctx->logfile = stderr;

    while ((retval = getopt(argc, argv, "o:c:d")) != -1) {
        switch (retval) {
        case 'o':
            usbmon_ctx->logfile = fopen(optarg, "w+");  
            break;
        case 'c':
            conf_file_name = optarg;
            break;
        case 'd':
            daemonize = 1;
            break;
        } 
    }

    if (usbmon_ctx->logfile == NULL) {
        fprintf(stderr, "Failed to create logfile\n");
        exit(EXIT_FAILURE);
    }

    LIST_INIT(&(usbmon_ctx->hub_list));
    LIST_INIT(&(usbmon_ctx->port_list));
    LIST_INIT(&(usbmon_ctx->timeout_list));

    if (conf_file_name != NULL &&
        usb_monitor_parse_config(usbmon_ctx, conf_file_name)) {
        fprintf(stderr, "Failed to read config file\n");
        fclose(usbmon_ctx->logfile);
        exit(EXIT_FAILURE);
    }
    
    retval = libusb_init(NULL);
    if (retval) {
        fprintf(stderr, "libusb failed with error %s\n",
                libusb_error_name(retval));
        fclose(usbmon_ctx->logfile);
        exit(EXIT_FAILURE);
    }

    //Start signal handler
    sig_handler.sa_handler = usb_monitor_signal_handler;

    if (sigaction(SIGUSR1, &sig_handler, NULL)) {
        fprintf(stderr, "Could not intall signal handler\n");
        exit(EXIT_FAILURE);
    }

    if (daemonize && daemon(1,1)) {
        fprintf(stderr, "Failed to start usb-monitor as daemon\n");
        fclose(usbmon_ctx->logfile);
        libusb_exit(NULL);
        exit(EXIT_FAILURE);
    }

    libusb_hotplug_register_callback(NULL,
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                     LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                     LIBUSB_HOTPLUG_ENUMERATE,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     LIBUSB_HOTPLUG_MATCH_ANY,
                                     usb_monitor_cb,
                                     usbmon_ctx, NULL);

    USB_DEBUG_PRINT(usbmon_ctx->logfile, "Initial state:\n");
    usb_monitor_print_ports(usbmon_ctx);

    gettimeofday(&last_restart, NULL);
    gettimeofday(&last_dev_check, NULL);
    //For now, just use the libusb wait-function as a basic event loop
    while (1) {
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);
        
        //Check if we have any pending timeouts
        usb_monitor_check_timeouts(usbmon_ctx);

        gettimeofday(&cur_time, NULL);

        //Do not run both checkes at the same time
        if (cur_time.tv_sec - last_dev_check.tv_sec > 30) {
            last_dev_check.tv_sec = cur_time.tv_sec;
            usb_helpers_check_devices(usbmon_ctx);
        } else if (cur_time.tv_sec - last_restart.tv_sec > 60) {
            last_restart.tv_sec = cur_time.tv_sec;
            usb_monitor_reset_all_ports(usbmon_ctx, 0);
        }
    }

    libusb_exit(NULL);
    exit(EXIT_SUCCESS);
}
