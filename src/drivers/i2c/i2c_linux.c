#include <csp/drivers/i2c_linux.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#if defined(__has_include)
#if __has_include(<i2c/smbus.h>)
#include <i2c/smbus.h>
#endif
#endif
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

#ifndef I2C_SMBUS_BLOCK_MAX
#define I2C_SMBUS_BLOCK_MAX 32
#endif

#ifndef i2c_smbus_write_i2c_block_data
static inline int i2c_smbus_access(int file, char read_write, uint8_t command, int size, union i2c_smbus_data * data) {
        struct i2c_smbus_ioctl_data args = {
                .read_write = read_write,
                .command = command,
                .size = size,
                .data = data,
        };

        return ioctl(file, I2C_SMBUS, &args);
}

static inline int i2c_smbus_write_i2c_block_data(int file, uint8_t command, uint8_t length, const uint8_t * values) {
        union i2c_smbus_data data;

        if (length > I2C_SMBUS_BLOCK_MAX) {
                length = I2C_SMBUS_BLOCK_MAX;
        }

        data.block[0] = length;
        memcpy(&data.block[1], values, length);

        return i2c_smbus_access(file, I2C_SMBUS_WRITE, command, I2C_SMBUS_I2C_BLOCK_DATA, &data);
}

static inline int i2c_smbus_read_i2c_block_data(int file, uint8_t command, uint8_t length, uint8_t * values) {
        union i2c_smbus_data data;

        if (length > I2C_SMBUS_BLOCK_MAX) {
                length = I2C_SMBUS_BLOCK_MAX;
        }

        data.block[0] = length;

        if (i2c_smbus_access(file, I2C_SMBUS_READ, command,
                             (length == I2C_SMBUS_BLOCK_MAX) ? I2C_SMBUS_I2C_BLOCK_BROKEN : I2C_SMBUS_I2C_BLOCK_DATA, &data) < 0) {
                return -1;
        }

        memcpy(values, &data.block[1], data.block[0]);
        return data.block[0];
}
#endif

/** Context for a Linux I2C interface. */
typedef struct {
        char name[CSP_IFLIST_NAME_MAX + 1];
        csp_iface_t iface;
        csp_i2c_interface_data_t ifdata;
        pthread_t rx_thread;
        pthread_mutex_t fd_lock;
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

        pthread_mutex_destroy(&ctx->fd_lock);
        free(ctx);
}

static int csp_i2c_linux_tx(void * driver_data, csp_packet_t * packet) {
        i2c_linux_ctx_t * ctx = driver_data;

        const uint8_t dest = packet->cfpid & 0x7F;

        if (packet->frame_length > I2C_SMBUS_BLOCK_MAX) {
                csp_print("%s[%s]: packet too large for SMBus write (%u > %u)\n", __func__, ctx->name, packet->frame_length,
                          I2C_SMBUS_BLOCK_MAX);
                csp_buffer_free(packet);
                return CSP_ERR_TX;
        }

        if (pthread_mutex_lock(&ctx->fd_lock) != 0) {
                csp_print("%s[%s]: failed to lock fd mutex\n", __func__, ctx->name);
                csp_buffer_free(packet);
                return CSP_ERR_TX;
        }

        if (ioctl(ctx->fd, I2C_SLAVE, dest) < 0) {
                csp_print("%s[%s]: ioctl(I2C_SLAVE) failed for address %u, error: %s\n", __func__, ctx->name, dest,
                          strerror(errno));
                pthread_mutex_unlock(&ctx->fd_lock);
                csp_buffer_free(packet);
                return CSP_ERR_TX;
        }

        const int res = i2c_smbus_write_i2c_block_data(ctx->fd, 0, packet->frame_length, packet->frame_begin);

        pthread_mutex_unlock(&ctx->fd_lock);

        if (res < 0) {
                csp_print("%s[%s]: i2c_smbus_write_i2c_block_data() failed, error: %s\n", __func__, ctx->name,
                          strerror(errno));
                csp_buffer_free(packet);
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
                const uint8_t max_rx = (buf_len > I2C_SMBUS_BLOCK_MAX) ? I2C_SMBUS_BLOCK_MAX : buf_len;

                if (pthread_mutex_lock(&ctx->fd_lock) != 0) {
                        csp_print("%s[%s]: failed to lock fd mutex\n", __func__, ctx->name);
                        csp_buffer_free(packet);
                        sleep(1);
                        continue;
                }

                if (ioctl(ctx->fd, I2C_SLAVE, ctx->address) < 0) {
                        csp_print("%s[%s]: ioctl(I2C_SLAVE) failed for address %u, error: %s\n", __func__, ctx->name, ctx->address, strerror(errno));
                        pthread_mutex_unlock(&ctx->fd_lock);
                        csp_buffer_free(packet);
                        sleep(1);
                        continue;
                }

                const int received = i2c_smbus_read_i2c_block_data(ctx->fd, 0, max_rx, packet->frame_begin);

                pthread_mutex_unlock(&ctx->fd_lock);

                if (received < 0) {
                        csp_print("%s[%s]: i2c_smbus_read_i2c_block_data() failed, error: %s\n", __func__, ctx->name, strerror(errno));
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
        pthread_mutex_init(&ctx->fd_lock, NULL);

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

