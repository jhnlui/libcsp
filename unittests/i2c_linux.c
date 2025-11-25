#include <check.h>

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <csp/csp.h>
#include <csp/drivers/i2c_linux.h>

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

#define I2C_STUB_ADDR 0x31

static bool path_exists(const char * path) {
        struct stat st;
        return (stat(path, &st) == 0);
}

static int ensure_stub_loaded(void) {
        if (path_exists("/sys/bus/i2c/drivers/i2c-stub")) {
                return 0;
        }

        int rc = system("modprobe i2c-stub chip_addr=" STRINGIFY(I2C_STUB_ADDR));
        (void)rc;
        return path_exists("/sys/bus/i2c/drivers/i2c-stub") ? 0 : -1;
}

static int parse_bus_number(const char * entry_name) {
        const char * dash = strchr(entry_name, '-');
        if (dash == NULL) {
                return -1;
        }
        return atoi(dash + 1);
}

static int find_stub_bus(void) {
        DIR * dir = opendir("/sys/bus/i2c/devices");
        if (dir == NULL) {
                return -1;
        }

        int bus_id = -1;
        struct dirent * de;
        while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, "i2c-", 4) != 0) {
                        continue;
                }

                char name_path[128];
                snprintf(name_path, sizeof(name_path), "/sys/bus/i2c/devices/%s/name", de->d_name);

                char adapter_name[64] = {0};
                int fd = open(name_path, O_RDONLY);
                if (fd >= 0) {
                        ssize_t rlen = read(fd, adapter_name, sizeof(adapter_name) - 1);
                        (void)rlen;
                        close(fd);
                }

                if (strstr(adapter_name, "stub") != NULL) {
                        bus_id = parse_bus_number(de->d_name);
                        break;
                }
        }

        closedir(dir);
        return bus_id;
}

static ssize_t read_stub_registers(int bus, uint8_t addr, uint8_t * buffer, size_t len) {
        char eeprom_path[128];
        snprintf(eeprom_path, sizeof(eeprom_path), "/sys/bus/i2c/devices/%d-00%02x/eeprom", bus, addr);

        int fd = open(eeprom_path, O_RDONLY);
        if (fd < 0) {
                return -1;
        }

        ssize_t bytes = read(fd, buffer, len);
        close(fd);
        return bytes;
}

static bool prepare_i2c_stub(char * device_path, size_t len, int * bus_id_out) {
        if (ensure_stub_loaded() != 0) {
                printf("i2c-stub kernel module unavailable, skipping test.\n");
                return false;
        }

        int bus = find_stub_bus();
        if (bus < 0) {
                printf("unable to locate i2c-stub adapter, skipping test.\n");
                return false;
        }

        snprintf(device_path, len, "/dev/i2c-%d", bus);
        if (access(device_path, R_OK | W_OK) != 0) {
                printf("%s not accessible, skipping test.\n", device_path);
                return false;
        }

        if (bus_id_out) {
                *bus_id_out = bus;
        }

        return true;
}

START_TEST(test_i2c_linux_stub_tx_writes_payload)
{
        char device_path[32];
        int bus_id = -1;

        if (!prepare_i2c_stub(device_path, sizeof(device_path), &bus_id)) {
                return;
        }

        csp_init();

        csp_iface_t * iface = NULL;
        int rc = csp_i2c_linux_open_and_add_interface(device_path, "i2c-linux-ut", 5, I2C_STUB_ADDR, &iface);
        ck_assert_int_eq(rc, CSP_ERR_NONE);
        ck_assert_ptr_nonnull(iface);

        const uint8_t payload[] = {0xAA, 0x55, 0xDE, 0xAD, 0xBE, 0xEF};
        const size_t payload_len = sizeof(payload);

        csp_packet_t * packet = csp_buffer_get(payload_len);
        ck_assert_ptr_nonnull(packet);

        memcpy(packet->frame_begin, payload, payload_len);
        packet->frame_length = payload_len;
        packet->cfpid = I2C_STUB_ADDR;

        csp_i2c_interface_data_t * ifdata = iface->interface_data;
        rc = ifdata->tx_func(iface->driver_data, packet);
        ck_assert_int_eq(rc, CSP_ERR_NONE);

        uint8_t received[sizeof(payload)] = {0};
        ssize_t got = read_stub_registers(bus_id, I2C_STUB_ADDR, received, sizeof(received));
        if (got < 0) {
                printf("unable to read stub registers; skipping verification.\n");
                csp_i2c_linux_stop(iface);
                return;
        }

        if ((size_t)got == payload_len + 1) {
                ck_assert_mem_eq(payload, &received[1], payload_len);
        } else {
                ck_assert_int_eq((size_t)got, payload_len);
                ck_assert_mem_eq(payload, received, payload_len);
        }

        ck_assert_int_eq(csp_i2c_linux_stop(iface), CSP_ERR_NONE);
}
END_TEST

Suite * i2c_linux_suite(void)
{
        Suite *s;
        TCase *tc_tx;

        s = suite_create("i2c-linux");
        tc_tx = tcase_create("tx");
        tcase_add_test(tc_tx, test_i2c_linux_stub_tx_writes_payload);
        suite_add_tcase(s, tc_tx);

        return s;
}

#else /* __linux__ */

Suite * i2c_linux_suite(void)
{
        return suite_create("i2c-linux");
}

#endif /* __linux__ */

