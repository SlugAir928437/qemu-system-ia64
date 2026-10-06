/*
 * Command-line overrides for board-default input devices.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

typedef struct InputMachine {
    const char *name;
    bool ps2;
} InputMachine;

static QTestState *input_start(const char *machine, const char *options)
{
    return qtest_initf("-machine %s,nvram=none,firmware=none -m 2G "
                       "-S -display none -serial none -net none %s",
                       machine, options);
}

static bool vpc_has_ps2(void)
{
    /* PS/2 devices may be built for other machines while VPC PS/2 is off. */
    QTestState *qts = input_start("ia64-vpc", "-nodefaults -preconfig");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'i8042','value':true}}");
    bool supported = qdict_haskey(response, "return");

    qtest_quit(qts);
    return supported;
}

static unsigned count_input_devices(QTestState *qts, const char *path,
                                     const char *type)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "{'execute':'qom-list','arguments':{'path':%s}}", path);
    g_autofree char *child_type = g_strdup_printf("child<%s>", type);
    QList *children = qdict_get_qlist(response, "return");
    QListEntry *entry;
    unsigned count = 0;

    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *property_type = qdict_get_str(child, "type");

        if (g_str_has_prefix(property_type, "child<")) {
            g_autofree char *child_path = g_strdup_printf(
                "%s/%s", path, qdict_get_str(child, "name"));

            if (g_str_equal(property_type, child_type) &&
                qtest_qom_get_bool(qts, child_path, "realized")) {
                count++;
            }
            count += count_input_devices(qts, child_path, type);
        }
    }
    return count;
}

static void assert_usb_input(QTestState *qts, unsigned keyboards,
                             unsigned mice, unsigned tablets)
{
    g_assert_cmpuint(count_input_devices(qts, "/machine", "usb-kbd"),
                     ==, keyboards);
    g_assert_cmpuint(count_input_devices(qts, "/machine", "usb-mouse"),
                     ==, mice);
    g_assert_cmpuint(count_input_devices(qts, "/machine", "usb-tablet"),
                     ==, tablets);
}

static void assert_pointer(QTestState *qts, const char *name, bool absolute)
{
    g_autoptr(QDict) response = qtest_qmp(qts, "{'execute':'query-mice'}");
    QList *mice = qdict_get_qlist(response, "return");
    QListEntry *entry;
    bool found = false;

    QLIST_FOREACH_ENTRY(mice, entry) {
        QDict *mouse = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(mouse, "name"), name)) {
            g_assert_cmpint(qdict_get_bool(mouse, "absolute"), ==, absolute);
            found = true;
        }
    }
    g_assert_true(found);
}

static void assert_ps2_input(QTestState *qts)
{
    g_assert_cmpuint(count_input_devices(qts, "/machine", "ps2-kbd"), ==, 1);
    g_assert_cmpuint(count_input_devices(qts, "/machine", "ps2-mouse"), ==, 1);
    assert_pointer(qts, "QEMU PS/2 Mouse", false);
}

static void test_usb_mouse(gconstpointer opaque)
{
    const InputMachine *machine = opaque;
    QTestState *qts = input_start(machine->name, "");

    assert_usb_input(qts, 1, 0, 1);
    assert_pointer(qts, "QEMU HID Tablet", true);
    qtest_quit(qts);

    qts = input_start(machine->name, "-use-usb-mouse-instead-of-tablet");
    assert_usb_input(qts, 1, 1, 0);
    assert_pointer(qts, "QEMU HID Mouse", false);
    qtest_quit(qts);

    qts = input_start(machine->name,
                      "-nodefaults -use-usb-mouse-instead-of-tablet");
    assert_usb_input(qts, 0, 0, 0);
    qtest_quit(qts);

    {
        g_autofree char *usb_off = g_strdup_printf("%s,usb=off", machine->name);

        qts = input_start(usb_off, "-use-usb-mouse-instead-of-tablet");
        assert_usb_input(qts, 0, 0, 0);
        qtest_quit(qts);
    }
}

static void test_force_ps2(gconstpointer opaque)
{
    const InputMachine *machine = opaque;
    static const char *options[] = {
        "-force-ps2-input",
        "-force-ps2-input -use-usb-mouse-instead-of-tablet",
        "-use-usb-mouse-instead-of-tablet -force-ps2-input",
        "-nodefaults -force-ps2-input",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(options); i++) {
        QTestState *qts = input_start(machine->name, options[i]);

        assert_usb_input(qts, 0, 0, 0);
        assert_ps2_input(qts);
        qtest_quit(qts);
    }
}

static void test_explicit_ps2(void)
{
    QTestState *qts = input_start("ia64-vpc,i8042=off",
                                  "-force-ps2-input -device i8042");

    assert_usb_input(qts, 0, 0, 0);
    assert_ps2_input(qts);
    qtest_quit(qts);
}

static void test_explicit_usb(gconstpointer opaque)
{
    const InputMachine *machine = opaque;
    static const char *devices[] = {
        "-device usb-kbd -device usb-mouse -device usb-tablet",
        "-usbdevice keyboard -usbdevice mouse -usbdevice tablet",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(devices); i++) {
        g_autofree char *options = g_strdup_printf(
            "-nodefaults -usb -use-usb-mouse-instead-of-tablet %s %s",
            machine->ps2 ? "-force-ps2-input" : "", devices[i]);
        QTestState *qts = input_start(machine->name, options);

        assert_usb_input(qts, 1, 1, 1);
        qtest_quit(qts);
    }
}

static void test_force_ps2_unavailable(void)
{
    static const char *machines[] = {
        "ia64-vpc,i8042=off,nvram=none,firmware=none", "none",
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        const char *argv[] = {
            qtest_qemu_binary(NULL), "-machine", machines[i],
            "-S", "-nodefaults", "-display", "none", "-force-ps2-input",
            NULL,
        };
        g_autofree char *stderr_text = NULL;
        g_autoptr(GError) error = NULL;
        int wait_status;

        g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                                   G_SPAWN_STDOUT_TO_DEV_NULL,
                                   NULL, NULL, NULL, &stderr_text,
                                   &wait_status, &error));
        g_assert_no_error(error);
        g_assert_true(WIFEXITED(wait_status));
        g_assert_cmpint(WEXITSTATUS(wait_status), ==, EXIT_FAILURE);
        g_assert_nonnull(strstr(stderr_text,
            "-force-ps2-input requires an enabled PS/2 controller"));
    }
}

static void ps2_outb(QTestState *qts, uint16_t port, uint8_t value)
{
    qtest_writeb(qts, IA64_LEGACY_IO_PORT_PA(port), value);
}

static uint8_t ps2_inb(QTestState *qts, uint16_t port)
{
    return qtest_readb(qts, IA64_LEGACY_IO_PORT_PA(port));
}

static void test_ps2_events(void)
{
    QTestState *qts = input_start("ia64-vpc,i8042=on", "-force-ps2-input");

    /* Enable untranslated keyboard input and the auxiliary port. */
    ps2_outb(qts, 0x64, 0x60);
    ps2_outb(qts, 0x60, 0x03);
    ps2_outb(qts, 0x60, 0xf4);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 0xfa);
    ps2_outb(qts, 0x64, 0xd4);
    ps2_outb(qts, 0x60, 0xf4);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 0xfa);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");

    qtest_qmp_assert_success(qts,
        "{'execute':'input-send-event','arguments':{'events':"
        "[{'type':'key','data':{'down':true,'key':"
        "{'type':'qcode','data':'a'}}}]}}");
    g_assert_cmphex(ps2_inb(qts, 0x64) & 0x21, ==, 0x01);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 0x1c);

    qtest_qmp_assert_success(qts,
        "{'execute':'input-send-event','arguments':{'events':"
        "[{'type':'rel','data':{'axis':'x','value':5}},"
        " {'type':'rel','data':{'axis':'y','value':-3}}]}}");
    g_assert_cmphex(ps2_inb(qts, 0x64) & 0x21, ==, 0x21);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 0x08);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 5);
    g_assert_cmphex(ps2_inb(qts, 0x60), ==, 3);
    g_assert_cmphex(ps2_inb(qts, 0x64) & 1, ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const InputMachine machines[] = {
        { "ia64-vpc", false },
        { "hp-i2000", true },
        { "hp-zx2000", false },
        { "hp-zx6000", false },
        { "hp-rx2660", false },
    };
    static const InputMachine vpc_ps2 = { "ia64-vpc,i8042=on", true };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        if (qtest_has_machine(machines[i].name)) {
            g_autofree char *usb_path = g_strdup_printf(
                "/default-input/%s/usb-mouse", machines[i].name);
            g_autofree char *explicit_path = g_strdup_printf(
                "/default-input/%s/explicit-usb", machines[i].name);

            qtest_add_data_func(usb_path, &machines[i], test_usb_mouse);
            qtest_add_data_func(explicit_path, &machines[i], test_explicit_usb);
            if (machines[i].ps2 && qtest_has_device("ps2-kbd")) {
                g_autofree char *ps2_path = g_strdup_printf(
                    "/default-input/%s/force-ps2", machines[i].name);

                qtest_add_data_func(ps2_path, &machines[i], test_force_ps2);
            }
        }
    }
    if (vpc_has_ps2()) {
        qtest_add_data_func("/default-input/ia64-vpc/force-ps2", &vpc_ps2,
                            test_force_ps2);
        qtest_add_data_func("/default-input/ia64-vpc/ps2-explicit-usb",
                            &vpc_ps2, test_explicit_usb);
        qtest_add_func("/default-input/ps2-events", test_ps2_events);
    }
    if (qtest_has_device("i8042")) {
        qtest_add_func("/default-input/explicit-ps2", test_explicit_ps2);
    }
    qtest_add_func("/default-input/force-ps2-unavailable",
                    test_force_ps2_unavailable);
    return g_test_run();
}
