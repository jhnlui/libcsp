#include <csp/drivers/i2c_linux.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <csp/csp.h>
#include <csp/csp_debug.h>
#include <csp/csp_id.h>

/** Context for a Linux I2C interface. */
typedef struct {
        char name[CSP_IFLIST_NAME_MAX + 1];
        csp_iface_t iface;
        csp_i2c_interface_data_t ifdata;
        pthread_t rx_thread;
        int fd;
        uint8_t address;
} i2c_linux_ctx_t;

static void i2c_linux_free(i2c_linux_ctx_t * ctx) {

        if (ctx == NULL) {
                return;
        }

        if (ctx->fd >= 0) {
                close(ctx->fd);
        }

        free(ctx);
}

static int csp_i2c_linux_tx(void * driver_data, csp_packet_t * packet) {
        i2c_linux_ctx_t * ctx = driver_data;

        const uint8_t dest = packet->cfpid & 0x7F;

        struct i2c_msg msg = {
                .addr = dest,
                .flags = 0,
                .len = packet->frame_length,
                .buf = packet->frame_begin,
        };

        struct i2c_rdwr_ioctl_data ioctl_data = {
                .msgs = &msg,
                .nmsgs = 1,
        };

        if (ioctl(ctx->fd, I2C_RDWR, &ioctl_data) < 0) {
                csp_print("%s[%s]: ioctl(I2C_RDWR) failed, error: %s\n", __func__, ctx->name, strerror(errno));
                return CSP_ERR_TX;
        }

        csp_buffer_free(packet);
        return CSP_ERR_NONE;
}

static void * i2c_linux_rx_thread(void * arg) {
        i2c_linux_ctx_t * ctx = arg;

        while (1) {
                csp_packet_t * packet = csp_buffer_get(0);
                if (packet == NULL) {
                        csp_print("%s[%s]: failed to obtain buffer\n", __func__, ctx->name);
                        sleep(1);
                        continue;
                }

                const int header_len = csp_id_setup_rx(packet);
                const size_t buf_len = header_len + CSP_BUFFER_SIZE;
                ssize_t received = read(ctx->fd, packet->frame_begin, buf_len);
                if (received < 0) {
                        if (errno == EAGAIN || errno == EINTR) {
                                csp_buffer_free(packet);
                                continue;
                        }
                        csp_print("%s[%s]: read() failed, error: %s\n", __func__, ctx->name, strerror(errno));
                        csp_buffer_free(packet);
                        sleep(1);
                        continue;
                }

                if (received == 0) {
                        csp_buffer_free(packet);
                        continue;
                }

                packet->frame_length = (uint16_t)received;
                csp_i2c_rx(&ctx->iface, packet, NULL);
        }

        /* Not reached */
        pthread_exit(NULL);
}

int csp_i2c_linux_open_and_add_interface(const char * device, const char * ifname, uint8_t node_id, uint8_t i2c_address, csp_iface_t ** return_iface) {
        if (ifname == NULL) {
                ifname = CSP_IF_I2C_DEFAULT_NAME;
        }

        csp_print("INIT %s: device: [%s], addr: %u, node: %u\n", ifname, device, i2c_address, node_id);

        i2c_linux_ctx_t * ctx = calloc(1, sizeof(*ctx));
        if (ctx == NULL) {
                return CSP_ERR_NOMEM;
        }
        ctx->fd = -1;
        ctx->address = i2c_address & 0x7F;

        strncpy(ctx->name, ifname, sizeof(ctx->name) - 1);
        ctx->iface.name = ctx->name;
        ctx->iface.addr = node_id;
        ctx->iface.interface_data = &ctx->ifdata;
        ctx->iface.driver_data = ctx;
        ctx->ifdata.tx_func = csp_i2c_linux_tx;

        ctx->fd = open(device, O_RDWR);
        if (ctx->fd < 0) {
                csp_print("%s[%s]: open() failed for %s, error: %s\n", __func__, ctx->name, device, strerror(errno));
                i2c_linux_free(ctx);
                return CSP_ERR_INVAL;
        }

        if (ioctl(ctx->fd, I2C_SLAVE, ctx->address) < 0) {
                csp_print("%s[%s]: ioctl(I2C_SLAVE) failed for address %u, error: %s\n", __func__, ctx->name, ctx->address, strerror(errno));
                i2c_linux_free(ctx);
                return CSP_ERR_INVAL;
        }

        if (csp_i2c_add_interface(&ctx->iface) != CSP_ERR_NONE) {
                i2c_linux_free(ctx);
                return CSP_ERR_INVAL;
        }

        int error = pthread_create(&ctx->rx_thread, NULL, i2c_linux_rx_thread, ctx);
        if (error != 0) {
                csp_print("%s[%s]: pthread_create() failed, error: %s\n", __func__, ctx->name, strerror(errno));
                i2c_linux_free(ctx);
                return CSP_ERR_DRIVER;
        }

        if (return_iface) {
                *return_iface = &ctx->iface;
        }

        return CSP_ERR_NONE;
}

csp_iface_t * csp_i2c_linux_init(const char * device, uint8_t node_id, uint8_t i2c_address) {
        csp_iface_t * return_iface = NULL;
        const int res = csp_i2c_linux_open_and_add_interface(device, CSP_IF_I2C_DEFAULT_NAME, node_id, i2c_address, &return_iface);
        return (res == CSP_ERR_NONE) ? return_iface : NULL;
}

int csp_i2c_linux_stop(csp_iface_t * iface) {
        if ((iface == NULL) || (iface->driver_data == NULL)) {
                return CSP_ERR_INVAL;
        }

        i2c_linux_ctx_t * ctx = iface->driver_data;

        int error = pthread_cancel(ctx->rx_thread);
        if (error != 0) {
                csp_print("%s[%s]: pthread_cancel() failed, error: %s\n", __func__, ctx->name, strerror(errno));
                return CSP_ERR_DRIVER;
        }

        error = pthread_join(ctx->rx_thread, NULL);
        if (error != 0) {
                csp_print("%s[%s]: pthread_join() failed, error: %s\n", __func__, ctx->name, strerror(errno));
                return CSP_ERR_DRIVER;
        }

        i2c_linux_free(ctx);
        return CSP_ERR_NONE;
}

