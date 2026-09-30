.. SPDX-License-Identifier: GPL-2.0-or-later

Device emulation source notice
==============================

The following public sources are technical references for
the device models that link to this notice:

* HP's `zx2000 Technical Reference Guide, 5969-3154
  <https://www.manualslib.com/manual/359342/Hp-Zx2000.html>`__,
  pages 1-1, 2-11, 3-12 and B-4–B-7, for the 900 MHz CPU, RAM limits,
  PCI device paths, CMD649, Intel 82540 and PDH components; `CPU QuickSpecs 11822
  <https://www.hpe.com/psnow/doc/c04283081>`__ for the 1.4 GHz/1.5 MB CPU.
* The published `zx2000 PCI configuration and Linux boot log
  <https://www.okqubit.net/machines/hinv/hp_zx2000.txt>`__ for PCI bus numbers,
  device identities, BAR assignments, onboard interrupt GSIs and UART addresses.
* HP's `zx1 MIO External Reference Specification, revision 1.0
  <https://parisc.docs.kernel.org/en/latest/_downloads/b1581469ed89d27cd7792b0bc9ae9c15/Zx1-mio.pdf>`__,
  sections 2.4, 2.5 and 3.3, for I/O port routing, address-range
  registers and rope configuration in the shared zx1 model.
* The `ACPI Specification
  <https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/06_Device_Configuration/Device_Configuration.html>`__,
  sections 6.2.2, 6.4.3.5 and 6.4.3.6, for bridge windows and UART resources;
  Linux's `IA-64 PCI implementation
  <https://github.com/torvalds/linux/blob/v2.6.12/arch/ia64/pci/pci.c>`__,
  ``add_io_space()``, for zero translation selecting legacy I/O space.
* Intel's `EFI Specification 1.10
  <https://www.intel.com/content/dam/doc/product-specification/efi-v1-10-specification.pdf>`__,
  section 5.2 (``AllocatePool()``), for eight-byte pool alignment.
* Microchip's `LPC47B27x Data Sheet, DS00002492A
  <https://ww1.microchip.com/downloads/en/DeviceDoc/00002492A.pdf>`__,
  sections 10.5–10.7 and tables 20-1, 20-4–20-8, for the parallel port's
  configuration, 16-byte FIFO, test mode and service interrupt thresholds.
  The port has no attached peripheral; DMA and EPP transfers are not
  implemented.
* IBM's `PC/XT 286 Technical Reference, August 1986
  <https://bitsavers.trailing-edge.com/pdf/ibm/pc/xt/68X2537_XT286_Technical_Reference_Aug86.pdf>`__,
  pages 4-9–4-10, 4-14 and 4-24–4-26, for PS/2 scan code set 3 key-type
  commands, input validation and default key types.
* Linux
  `sound/pci/cs4281.c <https://github.com/torvalds/linux/blob/master/sound/pci/cs4281.c>`__
  for CS4281 BA0 registers, the four DMA and FIFO channels, serial-slot
  routing, and sample-rate conversion.
* Linux
  `drivers/net/ethernet/broadcom/tg3.h <https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/broadcom/tg3.h>`__
  and
  `tg3.c <https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/broadcom/tg3.c>`__
  for BCM5701/BCM5704 registers, SRAM and mailbox layout, PHY access, DMA
  descriptors, packet offloads, status blocks, and interrupts.
* Broadcom's `BCM57XX Programmer's Guide, 57XX-PG105-R
  <https://datasheet.datasheetarchive.com/originals/library/Datasheets-ZSAA1/DSAZSAA00017932.pdf>`__,
  pages 232-240 and 325-326, for the MISC_HOST_CTRL byte-swap bit, target
  byte ordering and non-frame DMA word ordering, pages 103-106 for status
  block layout and tagged interrupt acknowledgement, and pages 379-380 and
  550 for MAC status and NVM command write-one-to-clear fields and link-change
  acknowledgement.  The illumos
  `bge_chip2.c
  <https://github.com/illumos/illumos-gate/blob/master/usr/src/uts/common/io/bge/bge_chip2.c>`__
  also documents and programs the byte order for native big-endian MMIO.
* Intel's `EHCI specification, revision 1.0
  <https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf>`__,
  sections 2.2.3, 2.2.5 and 4.2, for companion-controller numbering and port
  ownership and routing.
  NEC's `uPD720101 User's Manual, S16336EJ5V0UM
  <https://www.edbatalha.info/compaq-n610c/uPD720101%20User%20Manual%20S16336EJ5V0UM00.pdf>`__,
  pages 76-77, specifies the alternating OHCI routing of its five USB ports.
* Linux
  `drivers/scsi/qla1280.h <https://github.com/torvalds/linux/blob/master/drivers/scsi/qla1280.h>`__
  and
  `qla1280.c <https://github.com/torvalds/linux/blob/master/drivers/scsi/qla1280.c>`__
  for ISP12160 mailbox, target, queue and autosense controls, and the
  combined RISC reset/release command and queue-index reads during mailbox
  completion;
  `drivers/net/ethernet/intel/e100.c <https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/intel/e100.c>`__
  for 82550/82559 configuration byte 18 receive CRC and stripping controls.
* Intel's `8255x 10/100 Mbps Ethernet Controller Family Open Source Software
  Developer Manual <https://www.intel.com/content/dam/doc/manual/8255x-10-100-mbps-ethernet-controller-software-dev-manual.pdf>`__,
  section 6.4.3, for receive completion flags, type/length classification,
  and DMA write ordering, section 6.4.2.3 (configuration byte 18), for
  receive CRC transfer and padding stripping, and section 7.3.11, for the
  PHY equalizer register's NOP command.  Other equalizer commands are not
  implemented.
* Intel's `82557: A Guide to 82596 Compatibility, AP-368
  <https://manualzilla.com/doc/5929355/intel-82557-to-82596-compatibility-guide>`__,
  section 5.1, for receive memory structure compatibility with the 82596.
  Intel's `82596DX/SX datasheet
  <https://bitsavers.trailing-edge.com/components/intel/ethernet/i82596.pdf>`__,
  pages 54–59, describes flexible receive buffer descriptors.  The
  `MIT 6.828 network driver lab addendum
  <https://pdos.csail.mit.edu/6.828/2008/labs/lab6/lab6.html>`__
  documents the 8255x RBD layout and the total packet count in the RFD.
* NetBSD's `sys/dev/ic/isp.c
  <https://github.com/NetBSD/src/blob/trunk/sys/dev/ic/isp.c>`__
  (BSD-2-Clause) for the 32-LUN limit and per-LUN queue parameters of
  Ultra2/Ultra3 SCSI adapters, and `sys/dev/ic/ispreg.h
  <https://github.com/NetBSD/src/blob/trunk/sys/dev/ic/ispreg.h>`__
  for the HCCR command and status bits.  `sys/dev/ic/ispmbox.h
  <https://github.com/NetBSD/src/blob/trunk/sys/dev/ic/ispmbox.h>`__
  defines the IOCB transfer negotiation and timeout flags.
* Intel's `8254x Family of Gigabit Ethernet Controllers Software Developer's
  Manual <https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf>`__,
  sections 3.2.7 and 3.4.3, for receive and transmit interrupt timers,
  and sections 5.6.8–5.6.9 for EEPROM subsystem identifiers.
* XFree86's
  `460gxPCI.c <https://github.com/NetBSD/xsrc/blob/netbsd-5/xfree/xc/programs/Xserver/hw/xfree86/os-support/bus/460gxPCI.c>`__
  and
  `scanpci.c <https://github.com/NetBSD/xsrc/blob/netbsd-5/xfree/xc/programs/Xserver/hw/xfree86/etc/scanpci.c>`__
  for the Intel 460GX CBN, CBUSES, DEVNPRES, BUSNO and SUBNO registers.
* The public PCI ID Repository
  `pci.ids <https://github.com/pciutils/pciids/blob/master/pci.ids>`__
  for the HP RMP-3 management-function identities.  Linux
  `drivers/tty/serial/8250/8250_pci.c <https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_pci.c>`__
  and
  `include/linux/pci_ids.h <https://github.com/torvalds/linux/blob/master/include/linux/pci_ids.h>`__
  for the HP Diva RMP3 PCI identifiers and its single 16550 UART in BAR1.
* FreeBSD
  `sys/dev/mpt/mpilib/mpi.h <https://github.com/freebsd/freebsd-src/blob/main/sys/dev/mpt/mpilib/mpi.h>`__
  for the LSI Fusion-MPT interface definitions, and Linux
  `drivers/message/fusion/mptbase.c <https://github.com/torvalds/linux/blob/master/drivers/message/fusion/mptbase.c>`__
  for the IOC reset doorbell functions and transition to the READY state.
  LSI's `mpi_cnfg.h
  <https://github.com/torvalds/linux/blob/master/drivers/message/fusion/lsi/mpi_cnfg.h>`__,
  distributed with Linux,
  defines IO Unit Page 1's static RAID volume ID policy.

The IA-64 firmware's PCI controller handles and device paths follow the
`UEFI 2.11 Device Path Protocol
<https://uefi.org/specs/UEFI/2.11/10_Protocols_Device_Path_Protocol.html>`__
and
`PCI I/O Protocol
<https://uefi.org/specs/UEFI/2.11/14_Protocols_PCI_Bus_Support.html>`__.
The public EDK II
`PciDeviceSupport.c
<https://github.com/tianocore/edk2/blob/master/MdeModulePkg/Bus/Pci/PciBusDxe/PciDeviceSupport.c>`__
is an implementation reference for publishing a PCI controller handle with
both protocols.

The IA-64 firmware describes the emulated HP machines' console UART using
the revision 1 layout of Microsoft's `Serial Port Console Redirection Table
<https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/serial-port-console-redirection-table>`__
(January 2002).

On HP profiles, VGA-primary configurations include the UART as a secondary
console in ``ConOut``, ``ErrOut`` and HCDP, following the
`VSI OpenVMS Version 8.4-1H1 Installation and Upgrade Manual
<https://docs.vmssoftware.com/docs/VSI_OpenVMS_Installation_Manual.pdf>`__
(Chapter 2, "Selecting a Primary and Secondary Console", page 12).
