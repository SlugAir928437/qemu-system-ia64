/*
 * QTest testcase for USB OHCI controller
 *
 * Copyright (c) 2014 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "hw/usb/usb.h"
#include "libqtest-single.h"
#include "qemu/module.h"
#include "libqos/usb.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"

typedef struct QOHCI_PCI QOHCI_PCI;

struct QOHCI_PCI {
    QOSGraphObject obj;
    QPCIDevice dev;
};

#define OHCI_CONTROL          0x04
#define OHCI_COMMAND_STATUS   0x08
#define OHCI_INTR_STATUS      0x0c
#define OHCI_INTR_ENABLE      0x10
#define OHCI_INTR_DISABLE     0x14
#define OHCI_HCCA             0x18
#define OHCI_CONTROL_HEAD_ED  0x20
#define OHCI_RH_DESCRIPTOR_A  0x48
#define OHCI_RH_STATUS        0x50
#define OHCI_RH_PORT_STATUS_1 0x54
#define OHCI_USB_RESUME       0x40
#define OHCI_USB_OPERATIONAL  0x80
#define OHCI_USB_SUSPEND      0xc0
#define OHCI_CONTROL_CLE      (1U << 4)
#define OHCI_CONTROL_PLE      (1U << 2)
#define OHCI_COMMAND_CLF      (1U << 1)
#define OHCI_INTR_WD          (1U << 1)
#define OHCI_INTR_RD          (1U << 3)
#define OHCI_INTR_RHSC        (1U << 6)
#define OHCI_INTR_MIE         (1U << 31)
#define OHCI_PORT_CCS         (1U << 0)
#define OHCI_PORT_PES         (1U << 1)
#define OHCI_PORT_PSS         (1U << 2)
#define OHCI_PORT_POCI        (1U << 3)
#define OHCI_PORT_PRS         (1U << 4)
#define OHCI_PORT_CSC         (1U << 16)
#define OHCI_PORT_PSSC        (1U << 18)
#define OHCI_PORT_PRSC        (1U << 20)
#define OHCI_PORT_LSDA        (1U << 9)
#define OHCI_PORT_CHANGES     (0x1fU << 16)
#define OHCI_RH_NPS           (1U << 9)
#define OHCI_RESUME_SIGNAL_NS (20 * NANOSECONDS_PER_SECOND / 1000)
#define OHCI_RESUME_EOP_NS    (3 * NANOSECONDS_PER_SECOND / 1500000)
#define OHCI_RESUME_RECOVERY_NS (3 * NANOSECONDS_PER_SECOND / 1000)
#define OHCI_RESUME_SAVE_NS   (7 * NANOSECONDS_PER_SECOND / 1000)

static QOHCI_PCI *resume_ohci;
static QPCIBar resume_bar;

typedef struct OHCITestCase {
    void (*check)(void);
} OHCITestCase;

typedef struct OHCISnapshotData {
    char *tmpdir;
    char *disk_path;
} OHCISnapshotData;

static void ohci_snapshot_data_free(void *opaque)
{
    OHCISnapshotData *snapshot = opaque;

    g_assert_cmpint(g_unlink(snapshot->disk_path), ==, 0);
    g_assert_cmpint(g_rmdir(snapshot->tmpdir), ==, 0);
    g_free(snapshot->disk_path);
    g_free(snapshot->tmpdir);
    g_free(snapshot);
}

static void *ohci_snapshot_setup(GString *cmd_line, void *arg)
{
    g_autofree char *quoted_disk_path = NULL;
    g_autoptr(GError) error = NULL;
    OHCISnapshotData *snapshot;

    if (!have_qemu_img()) {
        return NULL;
    }

    snapshot = g_new0(OHCISnapshotData, 1);
    snapshot->tmpdir = g_dir_make_tmp("ohci-resume-savevm-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot->tmpdir);
    snapshot->disk_path = g_build_filename(snapshot->tmpdir,
                                            "snapshot.qcow2", NULL);
    g_assert_true(mkimg(snapshot->disk_path, "qcow2", 16));
    quoted_disk_path = g_shell_quote(snapshot->disk_path);
    g_string_append_printf(cmd_line,
                           " -drive file=%s,format=qcow2,if=none,id=snapshot",
                           quoted_disk_path);
    g_test_queue_destroy(ohci_snapshot_data_free, snapshot);
    return snapshot;
}

static void test_ohci_hotplug(void *obj, void *data, QGuestAllocator *alloc)
{
    usb_test_hotplug(global_qtest, "ohci", "1", NULL);
}

static uint32_t ohci_port_status(QPCIDevice *dev)
{
    return qpci_io_readl(dev, resume_bar, OHCI_RH_PORT_STATUS_1);
}

static void ohci_clear_rhsc(QPCIDevice *dev)
{
    qpci_io_writel(dev, resume_bar, OHCI_INTR_STATUS, OHCI_INTR_RHSC);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, 0);
}

static void ohci_prepare_connected_port(QPCIDevice *dev)
{
    uint32_t status = ohci_port_status(dev);

    qpci_io_writel(dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    g_assert_cmphex(status & OHCI_PORT_CCS, ==, OHCI_PORT_CCS);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1,
                   OHCI_PORT_CSC | OHCI_PORT_PES);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_CSC, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PES, ==, OHCI_PORT_PES);
    ohci_clear_rhsc(dev);
}

static void check_ohci_port_resume(void)
{
    QPCIDevice *dev = &resume_ohci->dev;
    uint32_t status;

    ohci_prepare_connected_port(dev);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, OHCI_INTR_RHSC);
    ohci_clear_rhsc(dev);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PES, ==, OHCI_PORT_PES);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, 0);

    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);

    qtest_clock_step(global_qtest,
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS - 1);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);

    qtest_clock_step(global_qtest, 1);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, OHCI_PORT_PSSC);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, OHCI_INTR_RHSC);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSSC);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
}

static void check_ohci_controller_resume(void)
{
    QPCIDevice *dev = &resume_ohci->dev;
    uint32_t status;

    ohci_prepare_connected_port(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSS, ==,
                    OHCI_PORT_PSS);
    ohci_clear_rhsc(dev);

    qpci_io_writel(dev, resume_bar, OHCI_CONTROL, OHCI_USB_SUSPEND);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    qpci_io_writel(dev, resume_bar, OHCI_CONTROL, OHCI_USB_RESUME);

    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PES, ==, OHCI_PORT_PES);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_CONTROL), ==,
                    OHCI_USB_RESUME);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, 0);

    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS +
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSSC, ==, 0);

    qpci_io_writel(dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    qtest_clock_step(global_qtest,
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSSC, ==,
                    OHCI_PORT_PSSC);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RD, ==, 0);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSSC);
}

static void check_ohci_reset_suspended_port(void)
{
    QPCIDevice *dev = &resume_ohci->dev;
    uint32_t status;

    ohci_prepare_connected_port(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSS, ==,
                    OHCI_PORT_PSS);
    ohci_clear_rhsc(dev);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PRS);

    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PES, ==, OHCI_PORT_PES);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PRSC, ==, OHCI_PORT_PRSC);
    g_assert_cmphex(qpci_io_readl(dev, resume_bar, OHCI_INTR_STATUS) &
                    OHCI_INTR_RHSC, ==, OHCI_INTR_RHSC);

    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS +
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSSC, ==, 0);
}

static void check_ohci_unplug_during_resume(void)
{
    QPCIDevice *dev = &resume_ohci->dev;

    ohci_prepare_connected_port(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    ohci_clear_rhsc(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSS, ==,
                    OHCI_PORT_PSS);
}

static void test_ohci_resume_case(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    QOHCI_PCI *ohci = obj;
    OHCITestCase *test = data;

    qpci_device_enable(&ohci->dev);
    resume_ohci = ohci;
    resume_bar = qpci_iomap(&ohci->dev, 0, NULL);
    usb_test_hotplug(global_qtest, "ohci", "1", test->check);
    g_assert_cmphex(ohci_port_status(&ohci->dev) &
                    (OHCI_PORT_CCS | OHCI_PORT_PES | OHCI_PORT_PSS), ==, 0);
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS +
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(&ohci->dev) & OHCI_PORT_PSSC, ==, 0);
    qpci_iounmap(&ohci->dev, resume_bar);
    resume_ohci = NULL;
}

static void test_ohci_resume_savevm(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QOHCI_PCI *ohci = obj;
    QPCIDevice *dev = &ohci->dev;
    g_autofree char *response = NULL;
    uint32_t status;

    if (!data) {
        g_test_skip("qemu-img is required for resume savevm testing");
        return;
    }

    qpci_device_enable(dev);
    resume_ohci = ohci;
    resume_bar = qpci_iomap(dev, 0, NULL);
    qtest_qmp_device_add(global_qtest, "usb-tablet", "usbdev1",
                         "{'port': '1', 'bus': 'ohci.0'}");
    ohci_prepare_connected_port(dev);

    response = qtest_hmp(global_qtest, "savevm resume-idle");
    g_assert_cmpstr(response, ==, "");
    g_clear_pointer(&response, g_free);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    ohci_clear_rhsc(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    qtest_clock_step(global_qtest, OHCI_RESUME_SAVE_NS);

    response = qtest_hmp(global_qtest, "loadvm resume-idle");
    g_assert_cmpstr(response, ==, "");
    g_clear_pointer(&response, g_free);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS +
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSSC, ==, 0);

    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_PSS);
    ohci_clear_rhsc(dev);
    qpci_io_writel(dev, resume_bar, OHCI_RH_PORT_STATUS_1, OHCI_PORT_POCI);
    qtest_clock_step(global_qtest, OHCI_RESUME_SAVE_NS);

    response = qtest_hmp(global_qtest, "savevm resume-pending");
    g_assert_cmpstr(response, ==, "");
    g_clear_pointer(&response, g_free);

    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS +
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
    g_assert_cmphex(ohci_port_status(dev) & OHCI_PORT_PSSC, ==,
                    OHCI_PORT_PSSC);

    response = qtest_hmp(global_qtest, "loadvm resume-pending");
    g_assert_cmpstr(response, ==, "");
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);

    qtest_clock_step(global_qtest,
                     OHCI_RESUME_SIGNAL_NS + OHCI_RESUME_EOP_NS +
                     OHCI_RESUME_RECOVERY_NS - OHCI_RESUME_SAVE_NS - 1);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, OHCI_PORT_PSS);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, 0);
    qtest_clock_step(global_qtest, 1);
    status = ohci_port_status(dev);
    g_assert_cmphex(status & OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & OHCI_PORT_PSSC, ==, OHCI_PORT_PSSC);

    qtest_qmp_device_del(global_qtest, "usbdev1");
    qpci_iounmap(dev, resume_bar);
    resume_ohci = NULL;
}

typedef struct OHCIWakeTest {
    QPCIDevice *dev;
    QGuestAllocator *alloc;
    uint64_t dma;
    bool tablet;
} OHCIWakeTest;

static uint32_t ohci_wake_port(OHCIWakeTest *test, unsigned int port)
{
    return qpci_io_readl(test->dev, resume_bar,
                         OHCI_RH_PORT_STATUS_1 + 4 * port);
}

static void ohci_wake_port_write(OHCIWakeTest *test, unsigned int port,
                                 uint32_t value)
{
    qpci_io_writel(test->dev, resume_bar,
                   OHCI_RH_PORT_STATUS_1 + 4 * port, value);
}

static uint32_t ohci_wake_interrupts(OHCIWakeTest *test)
{
    return qpci_io_readl(test->dev, resume_bar, OHCI_INTR_STATUS);
}

static bool ohci_wake_irq(OHCIWakeTest *test)
{
    return qpci_config_readw(test->dev, PCI_STATUS) & PCI_STATUS_INTERRUPT;
}

/* Submit a no-data control request through endpoint zero. */
static void ohci_wake_control_transfer(OHCIWakeTest *test, uint8_t address,
                                      uint8_t type, uint8_t request,
                                      uint16_t value, bool complete)
{
    uint32_t ed = test->dma + 0x100;
    uint32_t setup_td = ed + 0x10;
    uint32_t status_td = ed + 0x20;
    uint32_t tail_td = ed + 0x30;
    uint32_t packet = ed + 0x40;
    uint8_t setup[8] = { type, request, value, value >> 8 };
    uint32_t descriptors[] = {
        cpu_to_le32(address | (8 << 16)),
        cpu_to_le32(tail_td), cpu_to_le32(setup_td), 0,
        cpu_to_le32(0xf0000000U | (2 << 24)),
        cpu_to_le32(packet), cpu_to_le32(status_td), cpu_to_le32(packet + 7),
        cpu_to_le32(0xf0000000U | (3 << 24) | (2 << 19)),
        0, cpu_to_le32(tail_td), 0,
        0, 0, 0, 0,
    };

    qtest_memwrite(global_qtest, ed, descriptors, sizeof(descriptors));
    qtest_memwrite(global_qtest, packet, setup, sizeof(setup));
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL_HEAD_ED, ed);
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL,
                   OHCI_USB_OPERATIONAL | OHCI_CONTROL_CLE);
    qpci_io_writel(test->dev, resume_bar, OHCI_COMMAND_STATUS,
                   OHCI_COMMAND_CLF);
    qtest_clock_step(global_qtest, 2 * NANOSECONDS_PER_SECOND / 1000);
    qtest_memread(global_qtest, ed, descriptors, sizeof(descriptors));
    if (complete) {
        g_assert_cmphex(le32_to_cpu(descriptors[2]) & ~0xfU, ==, tail_td);
        g_assert_cmphex(le32_to_cpu(descriptors[2]) & 1, ==, 0);
        g_assert_cmphex(le32_to_cpu(descriptors[4]) >> 28, ==, 0);
        g_assert_cmphex(le32_to_cpu(descriptors[8]) >> 28, ==, 0);
    } else {
        g_assert_cmphex(le32_to_cpu(descriptors[2]), ==, setup_td);
        g_assert_cmphex(le32_to_cpu(descriptors[4]) >> 28, ==, 0xf);
        g_assert_cmphex(le32_to_cpu(descriptors[8]) >> 28, ==, 0xf);
    }
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL,
                   OHCI_USB_OPERATIONAL);
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL_HEAD_ED, 0);
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_STATUS, ~0U);
}

static void ohci_wake_control(OHCIWakeTest *test, uint8_t address,
                              uint8_t type, uint8_t request, uint16_t value)
{
    ohci_wake_control_transfer(test, address, type, request, value, true);
}

static void ohci_wake_init(OHCIWakeTest *test, QOHCI_PCI *ohci,
                           QGuestAllocator *alloc, bool tablet)
{
    *test = (OHCIWakeTest) {
        .dev = &ohci->dev,
        .alloc = alloc,
        .dma = guest_alloc(alloc, 4096),
        .tablet = tablet,
    };
    g_assert_cmpuint(test->dma, <=, UINT32_MAX - 4095);
    qpci_device_enable(test->dev);
    resume_bar = qpci_iomap(test->dev, 0, NULL);
    qtest_memset(global_qtest, test->dma, 0, 4096);
    qpci_io_writel(test->dev, resume_bar, OHCI_HCCA, test->dma);
    qtest_qmp_device_add(global_qtest, "usb-kbd", "wake-kbd",
                         "{'port': '1', 'bus': 'ohci.0'}");
    ohci_wake_port_write(test, 0, OHCI_PORT_PRS);
    ohci_wake_control(test, 0, 0, USB_REQ_SET_ADDRESS, 1);
    ohci_wake_control(test, 1, 0, USB_REQ_SET_CONFIGURATION, 1);
    ohci_wake_control(test, 1, 0, USB_REQ_SET_FEATURE,
                      USB_DEVICE_REMOTE_WAKEUP);
    if (tablet) {
        qtest_qmp_device_add(global_qtest, "usb-tablet", "wake-tablet",
                             "{'port': '2', 'bus': 'ohci.0'}");
        ohci_wake_port_write(test, 1, OHCI_PORT_PRS);
        ohci_wake_control(test, 0, 0, USB_REQ_SET_ADDRESS, 2);
        ohci_wake_control(test, 2, 0, USB_REQ_SET_CONFIGURATION, 1);
        ohci_wake_control(test, 2, 0, USB_REQ_SET_FEATURE,
                          USB_DEVICE_REMOTE_WAKEUP);
        ohci_wake_control(test, 2, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          0x0a, 0); /* HID SET_IDLE activates tablet input. */
        ohci_wake_port_write(test, 1, OHCI_PORT_CHANGES);
    }
    ohci_wake_port_write(test, 0, OHCI_PORT_CHANGES);
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_STATUS, ~0U);
}

static void ohci_wake_cleanup(OHCIWakeTest *test, bool keyboard_removed)
{
    if (!keyboard_removed) {
        qtest_qmp_device_del(global_qtest, "wake-kbd");
    }
    if (test->tablet) {
        qtest_qmp_device_del(global_qtest, "wake-tablet");
    }
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL, OHCI_USB_SUSPEND);
    qpci_iounmap(test->dev, resume_bar);
    guest_free(test->alloc, test->dma);
}

static void ohci_wake_key(bool down)
{
    qtest_qmp_assert_success(global_qtest,
        "{'execute':'input-send-event','arguments':{'events':"
        "[{'type':'key','data':{'down':%i,'key':"
        "{'type':'qcode','data':'a'}}}]}}", down);
}

static void ohci_keyboard_interrupt_transfer(OHCIWakeTest *test, bool down)
{
    uint32_t ed = test->dma + 0x200;
    uint32_t td = ed + 0x10;
    uint32_t tail = ed + 0x20;
    uint32_t buffer = ed + 0x30;
    uint32_t descriptors[] = {
        cpu_to_le32(1 | (1 << 7) | (2 << 11) | (8 << 16)),
        cpu_to_le32(tail), cpu_to_le32(td), 0,
        cpu_to_le32(0xf0000000U | (2 << 24)),
        cpu_to_le32(buffer), cpu_to_le32(tail), cpu_to_le32(buffer + 7),
        0, 0, 0, 0,
    };
    uint8_t report[8];
    uint8_t expected[8] = { 0, 0, down ? 4 : 0 };

    qtest_memwrite(global_qtest, ed, descriptors, sizeof(descriptors));
    qtest_memset(global_qtest, buffer, 0xa5, sizeof(report));
    for (unsigned int i = 0; i < 32; i++) {
        qtest_writel(global_qtest, test->dma + 4 * i, ed);
    }
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_ENABLE,
                   OHCI_INTR_MIE | OHCI_INTR_WD | OHCI_INTR_RHSC);
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL,
                   OHCI_USB_OPERATIONAL | OHCI_CONTROL_PLE);
    ohci_wake_key(down);
    qtest_clock_step(global_qtest, 2 * NANOSECONDS_PER_SECOND / 1000);

    qtest_memread(global_qtest, ed, descriptors, sizeof(descriptors));
    g_assert_cmphex(le32_to_cpu(descriptors[2]) & ~0xfU, ==, tail);
    g_assert_cmphex(le32_to_cpu(descriptors[2]) & 1, ==, 0);
    g_assert_cmphex(le32_to_cpu(descriptors[4]) >> 28, ==, 0);
    g_assert_cmphex(qtest_readl(global_qtest, test->dma + 0x84) & ~1U,
                    ==, td);
    qtest_memread(global_qtest, buffer, report, sizeof(report));
    g_assert_cmpmem(report, sizeof(report), expected, sizeof(expected));
    g_assert_cmphex(ohci_wake_interrupts(test) &
                    (OHCI_INTR_WD | OHCI_INTR_RHSC), ==, OHCI_INTR_WD);
    g_assert_true(ohci_wake_irq(test));
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_STATUS, OHCI_INTR_WD);
    g_assert_false(ohci_wake_irq(test));
    qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
}

static void test_ohci_no_power_switching(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    static const struct {
        uint32_t offset;
        uint32_t value;
    } writes[] = {
        { OHCI_RH_STATUS, 1 },
        { OHCI_RH_PORT_STATUS_1, OHCI_PORT_LSDA },
    };
    OHCIWakeTest test;
    uint32_t status;

    ohci_wake_init(&test, obj, alloc, false);
    g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_RH_DESCRIPTOR_A) &
                    OHCI_RH_NPS, ==, OHCI_RH_NPS);
    status = ohci_wake_port(&test, 0);
    g_assert_cmphex(status & (OHCI_PORT_CCS | OHCI_PORT_PES), ==,
                    OHCI_PORT_CCS | OHCI_PORT_PES);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_DISABLE, ~0U);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_ENABLE,
                   OHCI_INTR_MIE | OHCI_INTR_RHSC);

    for (unsigned int i = 0; i < G_N_ELEMENTS(writes); i++) {
        qpci_io_writel(test.dev, resume_bar, writes[i].offset, writes[i].value);
        g_assert_cmphex(ohci_wake_port(&test, 0), ==, status);
        g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RHSC, ==, 0);
        g_assert_false(ohci_wake_irq(&test));
        ohci_wake_control(&test, 1, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          0x0b, 0); /* HID SET_PROTOCOL: boot protocol. */
        ohci_keyboard_interrupt_transfer(&test, i == 0);
    }
    ohci_wake_cleanup(&test, false);
}

static void ohci_wake_tablet(void)
{
    qtest_qmp_assert_success(global_qtest,
        "{'execute':'input-send-event','arguments':{'events':"
        "[{'type':'abs','data':{'axis':'x','value':1234}}]}}");
}

static void ohci_wake_suspend(OHCIWakeTest *test, bool controller)
{
    ohci_wake_port_write(test, 0, OHCI_PORT_PSS);
    if (test->tablet) {
        ohci_wake_port_write(test, 1, OHCI_PORT_PSS);
    }
    qpci_io_writel(test->dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    if (controller) {
        qpci_io_writel(test->dev, resume_bar, OHCI_CONTROL, OHCI_USB_SUSPEND);
    }
    qtest_clock_step(global_qtest, 5 * NANOSECONDS_PER_SECOND / 1000);
}

static void ohci_wake_complete(OHCIWakeTest *test, unsigned int port)
{
    qtest_clock_step(global_qtest,
                     OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS - 1);
    g_assert_cmphex(ohci_wake_port(test, port) & OHCI_PORT_PSSC, ==, 0);
    qtest_clock_step(global_qtest, 1);
    g_assert_cmphex(ohci_wake_port(test, port) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSSC);
    g_assert_cmphex(ohci_wake_interrupts(test) & OHCI_INTR_RHSC, ==,
                    OHCI_INTR_RHSC);
}

static void test_ohci_remote_wakeup(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    static const unsigned int resume_ms[] = { 20, 33, 100 };
    OHCIWakeTest test;

    ohci_wake_init(&test, obj, alloc, false);
    for (unsigned int i = 0; i < G_N_ELEMENTS(resume_ms); i++) {
        ohci_wake_suspend(&test, true);
        qpci_io_writel(test.dev, resume_bar, OHCI_INTR_DISABLE, ~0U);
        ohci_wake_key(!(i & 1));
        g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                        OHCI_USB_RESUME);
        g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==,
                        OHCI_INTR_RD);
        g_assert_false(ohci_wake_irq(&test));
        qpci_io_writel(test.dev, resume_bar, OHCI_INTR_ENABLE,
                       OHCI_INTR_RD | OHCI_INTR_MIE);
        g_assert_true(ohci_wake_irq(&test));
        qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, OHCI_INTR_RD);
        g_assert_false(ohci_wake_irq(&test));
        qtest_clock_step(global_qtest,
                         resume_ms[i] * NANOSECONDS_PER_SECOND / 1000);
        g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                        OHCI_USB_RESUME);
        g_assert_cmphex(ohci_wake_port(&test, 0) &
                        (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, 0);
        g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RHSC, ==, 0);
        qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL,
                       OHCI_USB_OPERATIONAL);
        ohci_wake_complete(&test, 0);
        g_assert_false(ohci_wake_irq(&test));
        qpci_io_writel(test.dev, resume_bar, OHCI_INTR_ENABLE, OHCI_INTR_RHSC);
        g_assert_true(ohci_wake_irq(&test));

        /* Port status and interrupt status are independently acknowledged. */
        if (i & 1) {
            ohci_wake_port_write(&test, 0, OHCI_PORT_PSSC);
            g_assert_true(ohci_wake_irq(&test));
            qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS,
                           OHCI_INTR_RHSC);
        } else {
            qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS,
                           OHCI_INTR_RHSC);
            g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                            OHCI_PORT_PSSC);
            ohci_wake_port_write(&test, 0, OHCI_PORT_PSSC);
            /* Clearing a port status bit also changes HcRhPortStatus. */
            qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS,
                           OHCI_INTR_RHSC);
        }
        g_assert_false(ohci_wake_irq(&test));
        qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
        g_assert_cmphex(ohci_wake_interrupts(&test) &
                        (OHCI_INTR_RD | OHCI_INTR_RHSC), ==, 0);
    }
    ohci_wake_cleanup(&test, false);
}

static void test_ohci_remote_wakeup_traffic(void *obj, void *data,
                                           QGuestAllocator *alloc)
{
    OHCIWakeTest test;

    ohci_wake_init(&test, obj, alloc, false);
    ohci_wake_suspend(&test, false);
    ohci_wake_control_transfer(&test, 1, 0, USB_REQ_CLEAR_FEATURE,
                               USB_DEVICE_REMOTE_WAKEUP, false);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_SUSPEND);
    qtest_clock_step(global_qtest, 5 * NANOSECONDS_PER_SECOND / 1000);
    ohci_wake_key(true);
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    ohci_wake_control_transfer(&test, 1, 0, USB_REQ_CLEAR_FEATURE,
                               USB_DEVICE_REMOTE_WAKEUP, false);
    qtest_clock_step(global_qtest, OHCI_RESUME_EOP_NS +
                     OHCI_RESUME_RECOVERY_NS -
                     2 * NANOSECONDS_PER_SECOND / 1000);
    ohci_wake_control(&test, 1, 0, USB_REQ_CLEAR_FEATURE,
                      USB_DEVICE_REMOTE_WAKEUP);
    ohci_wake_cleanup(&test, false);
}

static void test_ohci_remote_wakeup_ports(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    OHCIWakeTest test;
    int mode = GPOINTER_TO_INT(data);

    ohci_wake_init(&test, obj, alloc, true);
    ohci_wake_suspend(&test, true);
    if (mode == 2) {
        qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_RESUME);
        g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==, 0);
    } else {
        ohci_wake_key(true);
    }
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    if (mode == 1) {
        ohci_wake_tablet();
    }
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    if (mode == 2) {
        qtest_clock_step(global_qtest,
                         OHCI_RESUME_EOP_NS + OHCI_RESUME_RECOVERY_NS);
        g_assert_cmphex(ohci_wake_port(&test, 0) &
                        (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSS);
        ohci_wake_key(true);
        qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    }
    ohci_wake_complete(&test, 0);
    g_assert_cmphex(ohci_wake_port(&test, 1) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSS);
    ohci_wake_port_write(&test, 0, OHCI_PORT_PSSC);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    if (mode == 1) {
        qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS - 1);
        g_assert_cmphex(ohci_wake_port(&test, 1) & OHCI_PORT_PSSC, ==, 0);
        qtest_clock_step(global_qtest, 1);
        g_assert_cmphex(ohci_wake_port(&test, 1) &
                        (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSSC);
    } else {
        ohci_wake_tablet();
        qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
        ohci_wake_complete(&test, 1);
    }
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==, 0);
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==, 0);
    ohci_wake_cleanup(&test, false);
}

static void test_ohci_remote_wakeup_disabled(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    OHCIWakeTest test;

    ohci_wake_init(&test, obj, alloc, false);
    ohci_wake_control(&test, 1, 0, USB_REQ_CLEAR_FEATURE,
                      USB_DEVICE_REMOTE_WAKEUP);
    ohci_wake_suspend(&test, true);
    ohci_wake_key(true);
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                    OHCI_USB_SUSPEND);
    g_assert_cmphex(ohci_wake_port(&test, 0) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSS);
    g_assert_cmphex(ohci_wake_interrupts(&test) &
                    (OHCI_INTR_RD | OHCI_INTR_RHSC), ==, 0);
    ohci_wake_cleanup(&test, false);
}

static void test_ohci_remote_wakeup_suspend_race(void *obj, void *data,
                                                QGuestAllocator *alloc)
{
    OHCIWakeTest test;
    int mode = GPOINTER_TO_INT(data);

    ohci_wake_init(&test, obj, alloc, false);
    ohci_wake_suspend(&test, false);
    if (mode) {
        ohci_wake_key(true);
        qtest_clock_step(global_qtest, mode == 1 ? OHCI_RESUME_SAVE_NS :
                         OHCI_RESUME_SIGNAL_NS + OHCI_RESUME_EOP_NS +
                         OHCI_RESUME_RECOVERY_NS);
        if (mode == 2) {
            g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                            OHCI_PORT_PSSC);
        }
    }
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_SUSPEND);
    if (!mode) {
        ohci_wake_key(true);
    }
    qtest_clock_step(global_qtest, 5 * NANOSECONDS_PER_SECOND / 1000 - 1);
    g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                    OHCI_USB_SUSPEND);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==, 0);
    qtest_clock_step(global_qtest, 1);
    g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                    OHCI_USB_RESUME);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==,
                    OHCI_INTR_RD);
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    if (mode == 2) {
        g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                        OHCI_PORT_PSSC);
    } else {
        ohci_wake_complete(&test, 0);
    }
    ohci_wake_cleanup(&test, false);
}

static void test_ohci_remote_wakeup_cancel(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    OHCIWakeTest test;
    int mode = GPOINTER_TO_INT(data);
    bool unplug = mode == 1;

    ohci_wake_init(&test, obj, alloc, false);
    ohci_wake_suspend(&test, true);
    ohci_wake_key(true);
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    if (unplug) {
        qtest_qmp_device_del(global_qtest, "wake-kbd");
    } else if (mode == 2) {
        ohci_wake_port_write(&test, 0, OHCI_PORT_CCS);
    } else {
        ohci_wake_port_write(&test, 0, OHCI_PORT_PRS);
    }
    ohci_wake_port_write(&test, 0, OHCI_PORT_CHANGES);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    g_assert_cmphex(ohci_wake_port(&test, 0) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, 0);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RHSC, ==, 0);
    if (mode) {
        g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PES, ==, 0);
    }
    ohci_wake_cleanup(&test, unplug);
}

static void ohci_wake_snapshot(const char *command)
{
    g_autofree char *response = qtest_hmp(global_qtest, "%s", command);

    g_assert_cmpstr(response, ==, "");
}

static void test_ohci_remote_wakeup_savevm(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    OHCIWakeTest test;
    int64_t recovery_remaining = OHCI_RESUME_EOP_NS +
                                OHCI_RESUME_RECOVERY_NS -
                                NANOSECONDS_PER_SECOND / 1000;

    if (!data) {
        g_test_skip("qemu-img is required for resume savevm testing");
        return;
    }
    ohci_wake_init(&test, obj, alloc, true);
    ohci_wake_suspend(&test, true);
    ohci_wake_key(true);
    qtest_clock_step(global_qtest, OHCI_RESUME_SAVE_NS);
    ohci_wake_snapshot("savevm remote-signal");
    qtest_clock_step(global_qtest, OHCI_RESUME_SIGNAL_NS);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    ohci_wake_complete(&test, 0);
    ohci_wake_snapshot("savevm remote-notification");
    ohci_wake_port_write(&test, 0, OHCI_PORT_PSSC);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, ~0U);

    ohci_wake_snapshot("loadvm remote-signal");
    g_assert_cmphex(qpci_io_readl(test.dev, resume_bar, OHCI_CONTROL), ==,
                    OHCI_USB_RESUME);
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RHSC, ==, 0);
    qpci_io_writel(test.dev, resume_bar, OHCI_CONTROL, OHCI_USB_OPERATIONAL);
    qtest_clock_step(global_qtest, NANOSECONDS_PER_SECOND / 1000);
    ohci_wake_snapshot("savevm remote-recovery");
    qtest_clock_step(global_qtest, recovery_remaining);
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                    OHCI_PORT_PSSC);

    ohci_wake_snapshot("loadvm remote-recovery");
    qtest_clock_step(global_qtest, recovery_remaining - 1);
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==, 0);
    qtest_clock_step(global_qtest, 1);
    g_assert_cmphex(ohci_wake_port(&test, 0) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSSC);
    ohci_wake_port_write(&test, 0, OHCI_PORT_PSSC);
    qpci_io_writel(test.dev, resume_bar, OHCI_INTR_STATUS, ~0U);
    ohci_wake_snapshot("loadvm remote-notification");
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                    OHCI_PORT_PSSC);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RHSC, ==,
                    OHCI_INTR_RHSC);
    g_assert_cmphex(ohci_wake_port(&test, 1) &
                    (OHCI_PORT_PSS | OHCI_PORT_PSSC), ==, OHCI_PORT_PSS);

    qpci_io_writel(test.dev, resume_bar, OHCI_COMMAND_STATUS, 1);
    g_assert_cmphex(ohci_wake_port(&test, 0) & OHCI_PORT_PSSC, ==,
                    OHCI_PORT_PSSC);
    ohci_wake_snapshot("savevm remote-hcr");
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==, 0);
    ohci_wake_snapshot("loadvm remote-hcr");
    qtest_clock_step(global_qtest, 100 * NANOSECONDS_PER_SECOND / 1000);
    g_assert_cmphex(ohci_wake_interrupts(&test) & OHCI_INTR_RD, ==, 0);
    ohci_wake_cleanup(&test, false);
}

static void *ohci_pci_get_driver(void *obj, const char *interface)
{
    QOHCI_PCI *ohci_pci = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &ohci_pci->dev;
    }

    fprintf(stderr, "%s not present in pci-ohci\n", interface);
    g_assert_not_reached();
}

static void *ohci_pci_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QOHCI_PCI *ohci_pci = g_new0(QOHCI_PCI, 1);

    qpci_device_init(&ohci_pci->dev, pci_bus, addr);
    ohci_pci->obj.get_driver = ohci_pci_get_driver;

    return &ohci_pci->obj;
}

static void ohci_pci_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,id=ohci",
    };
    QOSGraphEdgeOptions ia64_opts = {
        .extra_device_opts = "addr=07.0,id=ohci",
    };
    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });
    add_qpci_address(&ia64_opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(7, 0) });

    qos_node_create_driver("pci-ohci", ohci_pci_create);
    qos_node_consumes("pci-ohci", "pci-bus", &opts);
    qos_node_consumes("pci-ohci", "ia64-pci-bus", &ia64_opts);
    qos_node_produces("pci-ohci", "pci-device");
}

libqos_init(ohci_pci_register_nodes);

static void register_ohci_pci_test(void)
{
    static OHCITestCase port_resume = {
        .check = check_ohci_port_resume,
    };
    static OHCITestCase controller_resume = {
        .check = check_ohci_controller_resume,
    };
    static OHCITestCase reset_suspended_port = {
        .check = check_ohci_reset_suspended_port,
    };
    static OHCITestCase unplug_during_resume = {
        .check = check_ohci_unplug_during_resume,
    };
    static QOSGraphTestOptions port_resume_opts = {
        .arg = &port_resume,
    };
    static QOSGraphTestOptions controller_resume_opts = {
        .arg = &controller_resume,
    };
    static QOSGraphTestOptions reset_suspended_port_opts = {
        .arg = &reset_suspended_port,
    };
    static QOSGraphTestOptions unplug_during_resume_opts = {
        .arg = &unplug_during_resume,
    };
    static QOSGraphTestOptions resume_savevm_opts = {
        .before = ohci_snapshot_setup,
    };
    static QOSGraphTestOptions remote_late_port_opts = {
        .arg = GINT_TO_POINTER(1),
    };
    static QOSGraphTestOptions remote_host_resume_opts = {
        .arg = GINT_TO_POINTER(2),
    };
    static QOSGraphTestOptions remote_unplug_opts = {
        .arg = GINT_TO_POINTER(1),
    };
    static QOSGraphTestOptions remote_disable_opts = {
        .arg = GINT_TO_POINTER(2),
    };
    static QOSGraphTestOptions remote_pending_suspend_opts = {
        .arg = GINT_TO_POINTER(1),
    };
    static QOSGraphTestOptions remote_changed_suspend_opts = {
        .arg = GINT_TO_POINTER(2),
    };

    qos_add_test("ohci_pci-test-hotplug", "pci-ohci", test_ohci_hotplug, NULL);
    qos_add_test("ohci_pci-test-port-resume", "pci-ohci",
                 test_ohci_resume_case, &port_resume_opts);
    qos_add_test("ohci_pci-test-controller-resume", "pci-ohci",
                 test_ohci_resume_case, &controller_resume_opts);
    qos_add_test("ohci_pci-test-reset-suspended-port", "pci-ohci",
                 test_ohci_resume_case, &reset_suspended_port_opts);
    qos_add_test("ohci_pci-test-unplug-during-resume", "pci-ohci",
                 test_ohci_resume_case, &unplug_during_resume_opts);
    qos_add_test("ohci_pci-test-resume-savevm", "pci-ohci",
                 test_ohci_resume_savevm, &resume_savevm_opts);
    qos_add_test("ohci_pci-test-remote-wakeup", "pci-ohci",
                 test_ohci_remote_wakeup, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-traffic", "pci-ohci",
                 test_ohci_remote_wakeup_traffic, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-ports", "pci-ohci",
                 test_ohci_remote_wakeup_ports, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-late-port", "pci-ohci",
                 test_ohci_remote_wakeup_ports, &remote_late_port_opts);
    qos_add_test("ohci_pci-test-remote-wakeup-host-resume", "pci-ohci",
                 test_ohci_remote_wakeup_ports, &remote_host_resume_opts);
    qos_add_test("ohci_pci-test-remote-wakeup-disabled", "pci-ohci",
                 test_ohci_remote_wakeup_disabled, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-suspend-guard", "pci-ohci",
                 test_ohci_remote_wakeup_suspend_race, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-pending-suspend", "pci-ohci",
                 test_ohci_remote_wakeup_suspend_race,
                 &remote_pending_suspend_opts);
    qos_add_test("ohci_pci-test-remote-wakeup-changed-suspend", "pci-ohci",
                 test_ohci_remote_wakeup_suspend_race,
                 &remote_changed_suspend_opts);
    qos_add_test("ohci_pci-test-remote-wakeup-reset", "pci-ohci",
                 test_ohci_remote_wakeup_cancel, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-unplug", "pci-ohci",
                 test_ohci_remote_wakeup_cancel, &remote_unplug_opts);
    qos_add_test("ohci_pci-test-remote-wakeup-port-disable", "pci-ohci",
                 test_ohci_remote_wakeup_cancel, &remote_disable_opts);
    qos_add_test("ohci_pci-test-no-power-switching", "pci-ohci",
                 test_ohci_no_power_switching, NULL);
    qos_add_test("ohci_pci-test-remote-wakeup-savevm", "pci-ohci",
                 test_ohci_remote_wakeup_savevm, &resume_savevm_opts);
}

libqos_init(register_ohci_pci_test);
