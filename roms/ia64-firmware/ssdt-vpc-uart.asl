// SPDX-License-Identifier: GPL-2.0-or-later

DefinitionBlock ("", "SSDT", 2, "QEMU  ", "VPCUART ", 0x00000001)
{
    External (\_SB.PCI0, DeviceObj)

    Scope (\_SB.PCI0)
    {
        Device (UAR0)
        {
            Name (_HID, EisaId ("PNP0501"))
            Name (_UID, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x03F8, 0x03F8, 1, 8)
                IRQNoFlags () {4}
            })
        }
    }
}
