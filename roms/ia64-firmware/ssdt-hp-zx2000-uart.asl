// SPDX-License-Identifier: GPL-2.0-or-later
// Default UART resources for the AML emitter's regression test.

DefinitionBlock ("", "SSDT", 2, "QEMU  ", "ZX2UART ", 0x00000001)
{
    External (\_SB.SBA0.PCI2, DeviceObj)

    Scope (\_SB.SBA0.PCI2)
    {
        Device (UAR0)
        {
            Name (_HID, EisaId ("PNP0501"))
            Name (_UID, 0)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                DWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0, 0xFF5E0000, 0xFF5E0007, 0, 8)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                    {45}
            })
        }

        Device (UAR1)
        {
            Name (_HID, EisaId ("PNP0501"))
            Name (_UID, 1)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                DWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0, 0xFF5E2000, 0xFF5E2007, 0, 8)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                    {46}
            })
        }
    }
}
