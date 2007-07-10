/*
 * This file is part of the LinuxBIOS project.
 *
 * Copyright (C) 2007 Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <types.h>
#include <lib.h>
#include <console.h>
#include <post_code.h>
#include <device/device.h>
#include <device/pci.h>
#include <string.h>
#include <msr.h>
#include <io.h>
#include <amd_geodelx.h>
#include <cpu.h>
#include <device/pci_ids.h>
#include <statictree.h>
#include "cs5536.h"

extern void setup_i8259(void);

struct msrinit {
	u32 msrnum;
	struct msr msr;
};

/* Master configuration register for bus masters */
struct msrinit SB_MASTER_CONF_TABLE[] = {
	{USB2_SB_GLD_MSR_CONF, {.hi = 0,.lo = 0x00008f000}},
	{ATA_SB_GLD_MSR_CONF,  {.hi = 0,.lo = 0x00048f000}},
	{AC97_SB_GLD_MSR_CONF, {.hi = 0,.lo = 0x00008f000}},
	{MDD_SB_GLD_MSR_CONF,  {.hi = 0,.lo = 0x00000f000}},
	{0, {0, 0}}
};

/* CS5536 clock gating */
struct msrinit CS5536_CLOCK_GATING_TABLE[] = {
	/* MSR                Setting */
	{GLIU_SB_GLD_MSR_PM,  {.hi = 0,.lo = 0x000000004}},
	{GLPCI_SB_GLD_MSR_PM, {.hi = 0,.lo = 0x000000005}},
	{GLCP_SB_GLD_MSR_PM,  {.hi = 0,.lo = 0x000000004}},
	{MDD_SB_GLD_MSR_PM,   {.hi = 0,.lo = 0x050554111}},	/*  SMBus clock gating errata (PBZ 2226 & SiBZ 3977) */
	{ATA_SB_GLD_MSR_PM,   {.hi = 0,.lo = 0x000000005}},
	{AC97_SB_GLD_MSR_PM,  {.hi = 0,.lo = 0x000000005}},
	{0, {0, 0}}
};

struct acpiinit {
	u16 ioreg;
	u32 regdata;
};

struct acpiinit acpi_init_table[] = {
	{ACPI_IO_BASE + 0x00, 0x01000000},
	{ACPI_IO_BASE + 0x08, 0x00000000},
	{ACPI_IO_BASE + 0x0C, 0x00000000},
	{ACPI_IO_BASE + 0x1C, 0x00000000},
	{ACPI_IO_BASE + 0x18, 0xFFFFFFFF},
	{ACPI_IO_BASE + 0x00, 0x0000FFFF},
	{PMS_IO_BASE + PM_SCLK, 0x000000E00},
	{PMS_IO_BASE + PM_SED,  0x000004601},
	{PMS_IO_BASE + PM_SIDD, 0x000008C02},
	{PMS_IO_BASE + PM_WKD,  0x0000000A0},
	{PMS_IO_BASE + PM_WKXD, 0x0000000A0},
	{0, 0}
};

struct FLASH_DEVICE {
	unsigned char fType;		/* Flash type: NOR or NAND */
	unsigned char fInterface;	/* Flash interface: I/O or memory */
	unsigned long fMask;		/* Flash size/mask */
};

struct FLASH_DEVICE FlashInitTable[] = {
	{FLASH_TYPE_NAND, FLASH_IF_MEM, FLASH_MEM_4K},	/* CS0, or Flash Device 0 */
	{FLASH_TYPE_NONE, 0, 0},	/* CS1, or Flash Device 1 */
	{FLASH_TYPE_NONE, 0, 0},	/* CS2, or Flash Device 2 */
	{FLASH_TYPE_NONE, 0, 0},	/* CS3, or Flash Device 3 */
};

u32 FlashPort[] = {
	MDD_LBAR_FLSH0,
	MDD_LBAR_FLSH1,
	MDD_LBAR_FLSH2,
	MDD_LBAR_FLSH3
};

/**
 * Program ACPI LBAR and initialize ACPI registers.
 */
static void pm_chipset_init(void)
{
	u32 val = 0;
	u16 port;

	port = (PMS_IO_BASE + 0x010);
	val = 0x0E00;		/*  1ms */
	outl(val, port);

	/* PM_WKXD */
	/* Make sure bits[3:0]=0000b to clear the saved Sx state. */
	port = (PMS_IO_BASE + PM_WKXD);
	val = 0x0A0;		/*  5ms */
	outl(val, port);

	/* PM_WKD */
	port = (PMS_IO_BASE + PM_WKD);
	outl(val, port);

	/* PM_SED */
	port = (PMS_IO_BASE + PM_SED);
	val = 0x04601;		/*  5ms, # of 3.57954MHz clock edges */
	outl(val, port);

	/* PM_SIDD */
	port = (PMS_IO_BASE + PM_SIDD);
	val = 0x08C02;		/*  10ms, # of 3.57954MHz clock edges */
	outl(val, port);
}

/**
 * Flash LBARs need to be setup before VSA init so the PCI BARs have
 * correct size info. Call this routine only if flash needs to be
 * configured (don't call it if you want IDE).
 */
static void chipset_flash_setup(void)
{
	int i;
	struct msr msr;
	int numEnabled = 0;

	printk(BIOS_DEBUG, "chipset_flash_setup: Start\n");
	for (i = 0; i < ARRAY_SIZE(FlashInitTable); i++) {
		if (FlashInitTable[i].fType != FLASH_TYPE_NONE) {
			printk(BIOS_DEBUG, "Enable CS%d\n", i);
			/* We need to configure the memory/IO mask. */
			msr = rdmsr(FlashPort[i]);
			msr.hi = 0;	/* Start with "enabled" bit clear. */
			if (FlashInitTable[i].fType == FLASH_TYPE_NAND)
				msr.hi |= 0x00000002;
			else
				msr.hi &= ~0x00000002;
			if (FlashInitTable[i].fInterface == FLASH_IF_MEM)
				msr.hi |= 0x00000004;
			else
				msr.hi &= ~0x00000004;
			msr.hi |= FlashInitTable[i].fMask;
			printk(BIOS_DEBUG, "MSR(0x%08X, %08X_%08X)\n",
			       FlashPort[i], msr.hi, msr.lo);
			wrmsr(FlashPort[i], msr);

			/* Now write-enable the device. */
			msr = rdmsr(MDD_NORF_CNTRL);
			msr.lo |= (1 << i);
			printk(BIOS_DEBUG, "MSR(0x%08X, %08X_%08X)\n",
			       MDD_NORF_CNTRL, msr.hi, msr.lo);
			wrmsr(MDD_NORF_CNTRL, msr);

			/* Update the number enabled. */
			numEnabled++;
		}
	}
	printk(BIOS_DEBUG, "chipset_flash_setup: Finish\n");
}

/**
 * Use this in the event that you have a FLASH part instead of an IDE drive.
 * Run after VSA init to enable the flash PCI device header.
 */
static void enable_ide_nand_flash_header(void)
{
	/* Tell VSA to use FLASH PCI header. Not IDE header. */
	outl(0x80007A40, 0xCF8);
	outl(0xDEADBEEF, 0xCFC);
}

#define RTC_CENTURY	0x32
#define RTC_DOMA	0x3D
#define RTC_MONA	0x3E

/**
 * Standard init function for the LPC bus.
 *
 * Sets up the "serial irq" interrupt, which is NOT the same as serial
 * interrupt, and also enables DMA from the LPC bus. Configures the PC clock,
 * enables RTC and ISA DMA.
 *
 * @param sb Southbridge config structure.
 */
static void lpc_init(struct southbridge_amd_cs5536_config *sb)
{
	struct msr msr;

	if (sb->lpc_serirq_enable) {
		msr.lo = sb->lpc_serirq_enable;
		msr.hi = 0;
		wrmsr(MDD_IRQM_LPC, msr);
		if (sb->lpc_serirq_polarity) {
			msr.lo = sb->lpc_serirq_polarity << 16;
			msr.lo |= (sb->lpc_serirq_mode << 6) | (1 << 7);	/* Enable */
			msr.hi = 0;
			wrmsr(MDD_LPC_SIRQ, msr);
		}
	}

	/* Allow DMA from LPC. */
	msr = rdmsr(MDD_DMA_MAP);
	msr.lo = 0x7777;
	wrmsr(MDD_DMA_MAP, msr);

	/* Enable the RTC/CMOS century byte at address 0x32. */
	msr = rdmsr(MDD_RTC_CENTURY_OFFSET);
	msr.lo = RTC_CENTURY;
	wrmsr(MDD_RTC_CENTURY_OFFSET, msr);

	/* Enable the RTC/CMOS day of month and month alarms. */
	msr = rdmsr(MDD_RTC_DOMA_IND);
	msr.lo = RTC_DOMA;
	wrmsr(MDD_RTC_DOMA_IND, msr);

	msr = rdmsr(MDD_RTC_MONA_IND);
	msr.lo = RTC_MONA;
	wrmsr(MDD_RTC_MONA_IND, msr);

	rtc_init(0);

	isa_dma_init();
}

/**
 * Depending on settings in the config struct, enable COM1 or COM2 or both.
 *
 * If the enable is NOT set, the UARTs are explicitly disabled, which is
 * required if (e.g.) there is a Super I/O attached that does COM1 or COM2.
 *
 * @param sb Southbridge config structure.
 */
static void uarts_init(struct southbridge_amd_cs5536_config *sb)
{
	struct msr msr;
	u16 addr = 0;
	u32 gpio_addr;
	struct device *dev;

	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_ISA, 0);
	gpio_addr = pci_read_config32(dev, PCI_BASE_ADDRESS_1);
	gpio_addr &= ~1;	/* Clear I/O bit */
	printk(BIOS_DEBUG, "GPIO_ADDR: %08X\n", gpio_addr);

	/* This could be extended to support IR modes. */

	/* COM1 */
	if (sb->com1_enable) {
		/* Set the address. */
		switch (sb->com1_address) {
		case 0x3F8:
			addr = 7;
			break;
		case 0x3E8:
			addr = 6;
			break;
		case 0x2F8:
			addr = 5;
			break;
		case 0x2E8:
			addr = 4;
			break;
		}
		msr = rdmsr(MDD_LEG_IO);
		msr.lo |= addr << 16;
		wrmsr(MDD_LEG_IO, msr);

		/* Set the IRQ. */
		msr = rdmsr(MDD_IRQM_YHIGH);
		msr.lo |= sb->com1_irq << 24;
		wrmsr(MDD_IRQM_YHIGH, msr);

		/* GPIO8 - UART1_TX */
		/* Set: Output Enable (0x4) */
		outl(GPIOL_8_SET, gpio_addr + GPIOL_OUTPUT_ENABLE);
		/* Set: OUTAUX1 Select (0x10) */
		outl(GPIOL_8_SET, gpio_addr + GPIOL_OUT_AUX1_SELECT);

		/* GPIO8 - UART1_RX */
		/* Set: Input Enable (0x20) */
		outl(GPIOL_9_SET, gpio_addr + GPIOL_INPUT_ENABLE);
		/* Set: INAUX1 Select (0x34) */
		outl(GPIOL_9_SET, gpio_addr + GPIOL_IN_AUX1_SELECT);

		/* Set: GPIO 8 + 9 Pull Up (0x18) */
		outl(GPIOL_8_SET | GPIOL_9_SET,
		     gpio_addr + GPIOL_PULLUP_ENABLE);

		/* Enable COM1.
		 *
		 * Bit 1 = device enable
		 * Bit 4 = allow access to the upper banks
		 */
		msr.lo = (1 << 4) | (1 << 1);
		msr.hi = 0;
		wrmsr(MDD_UART1_CONF, msr);
	} else {
		/* Reset and disable COM1. */
		msr = rdmsr(MDD_UART1_CONF);
		msr.lo = 1;			/* Reset */
		wrmsr(MDD_UART1_CONF, msr);
		msr.lo = 0;			/* Disabled */
		wrmsr(MDD_UART1_CONF, msr);

		/* Disable the IRQ. */
		msr = rdmsr(MDD_LEG_IO);
		msr.lo &= ~(0xF << 16);
		wrmsr(MDD_LEG_IO, msr);
	}

	/* COM2 */
	if (sb->com2_enable) {
		switch (sb->com2_address) {
		case 0x3F8:
			addr = 7;
			break;
		case 0x3E8:
			addr = 6;
			break;
		case 0x2F8:
			addr = 5;
			break;
		case 0x2E8:
			addr = 4;
			break;
		}
		msr = rdmsr(MDD_LEG_IO);
		msr.lo |= addr << 20;
		wrmsr(MDD_LEG_IO, msr);

		/* Set the IRQ. */
		msr = rdmsr(MDD_IRQM_YHIGH);
		msr.lo |= sb->com2_irq << 28;
		wrmsr(MDD_IRQM_YHIGH, msr);

		/* GPIO3 - UART2_RX */
		/* Set: Output Enable (0x4) */
		outl(GPIOL_3_SET, gpio_addr + GPIOL_OUTPUT_ENABLE);
		/* Set: OUTAUX1 Select (0x10) */
		outl(GPIOL_3_SET, gpio_addr + GPIOL_OUT_AUX1_SELECT);

		/* GPIO4 - UART2_TX */
		/* Set: Input Enable (0x20) */
		outl(GPIOL_4_SET, gpio_addr + GPIOL_INPUT_ENABLE);
		/* Set: INAUX1 Select (0x34) */
		outl(GPIOL_4_SET, gpio_addr + GPIOL_IN_AUX1_SELECT);

		/* Set: GPIO 3 + 3 Pull Up (0x18) */
		outl(GPIOL_3_SET | GPIOL_4_SET,
		     gpio_addr + GPIOL_PULLUP_ENABLE);

		/* Enable COM2.
		 *
		 * Bit 1 = device enable
		 * Bit 4 = allow access to the upper banks
		 */
		msr.lo = (1 << 4) | (1 << 1);
		msr.hi = 0;
		wrmsr(MDD_UART2_CONF, msr);
	} else {
		/* Reset and disable COM2. */
		msr = rdmsr(MDD_UART2_CONF);
		msr.lo = 1;			/* Reset */
		wrmsr(MDD_UART2_CONF, msr);
		msr.lo = 0;			/* Disabled */
		wrmsr(MDD_UART2_CONF, msr);

		/* Disable the IRQ. */
		msr = rdmsr(MDD_LEG_IO);
		msr.lo &= ~(0xF << 20);
		wrmsr(MDD_LEG_IO, msr);
	}
}

#define HCCPARAMS		0x08
#define IPREG04			0xA0
#define USB_HCCPW_SET		(1 << 1)
#define UOCCAP			0x00
#define APU_SET			(1 << 15)
#define UOCMUX			0x04
#define PMUX_HOST		0x02
#define PMUX_DEVICE		0x03
#define PUEN_SET		(1 << 2)
#define UDCDEVCTL		0x404
#define UDC_SD_SET		(1 << 10)
#define UOCCTL			0x0C
#define PADEN_SET		(1 << 7)

/**
 * Depending on settings in the config struct, manage USB setup.
 *
 * @param sb Southbridge config structure.
 */
static void enable_USB_port4(struct southbridge_amd_cs5536_config *sb)
{
	u32 *bar;
	struct msr msr;
	struct device *dev;

	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_EHCI, 0);
	if (dev) {
		/* Serial short detect enable */
		msr = rdmsr(USB2_SB_GLD_MSR_CONF);
		msr.hi |= USB2_UPPER_SSDEN_SET;
		wrmsr(USB2_SB_GLD_MSR_CONF, msr);

		/* Write to clear diag register. */
		wrmsr(USB2_SB_GLD_MSR_DIAG, rdmsr(USB2_SB_GLD_MSR_DIAG));

		bar = (u32 *) pci_read_config32(dev, PCI_BASE_ADDRESS_0);

		/* Make HCCPARAMS writable. */
		*(bar + IPREG04) |= USB_HCCPW_SET;

		/* EECP=50h, IST=01h, ASPC=1 */
		*(bar + HCCPARAMS) = 0x00005012;
	}

	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_OTG, 0);
	if (dev) {
		bar = (u32 *) pci_read_config32(dev, PCI_BASE_ADDRESS_0);

		*(bar + UOCMUX) &= PUEN_SET;

		/* Host or Device? */
		if (sb->enable_USBP4_device) {
			*(bar + UOCMUX) |= PMUX_DEVICE;
		} else {
			*(bar + UOCMUX) |= PMUX_HOST;
		}

		/* Overcurrent configuration */
		if (sb->enable_USBP4_overcurrent) {
			*(bar + UOCCAP) |= sb->enable_USBP4_overcurrent;
		}
	}

	/* PBz#6466: If the UOC(OTG) device, port 4, is configured as a
	 * device, then perform the following sequence:
	 *  - Set SD bit in DEVCTRL udc register
	 *  - Set PADEN (former OTGPADEN) bit in uoc register
	 *  - Set APU bit in uoc register
	 */
	if (sb->enable_USBP4_device) {
		dev = dev_find_device(PCI_VENDOR_ID_AMD,
				      PCI_DEVICE_ID_AMD_CS5536_UDC, 0);
		if (dev) {
			bar = (u32 *)pci_read_config32(dev, PCI_BASE_ADDRESS_0);
			*(bar + UDCDEVCTL) |= UDC_SD_SET;
		}

		dev = dev_find_device(PCI_VENDOR_ID_AMD,
				      PCI_DEVICE_ID_AMD_CS5536_OTG, 0);
		if (dev) {
			bar = (u32 *)pci_read_config32(dev, PCI_BASE_ADDRESS_0);
			*(bar + UOCCTL) |= PADEN_SET;
			*(bar + UOCCAP) |= APU_SET;
		}
	}

	/* Disable virtual PCI UDC and OTG headers. */
	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_UDC, 0);
	if (dev) {
		pci_write_config32(dev, 0x7C, 0xDEADBEEF);
	}

	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_OTG, 0);
	if (dev) {
		pci_write_config32(dev, 0x7C, 0xDEADBEEF);
	}
}

/** 
 * This function initializes a lot of nasty bits needed for phase 2.
 *
 * Can this function run before vsm is set up, or is it required for vsm?
 * The order here is a little hard to figure out.
 *
 * This function is in an odd place. We need to see about moving it to
 * geodelx.c. But for now, let's get things working and put a #warning in.
 */
void chipsetinit(void)
{
	struct device *dev;
	struct msr msr;
	u32 msrnum;
	struct southbridge_amd_cs5536_config *sb;
	struct msrinit *csi;

	post_code(P80_CHIPSET_INIT);
	dev = dev_find_device(PCI_VENDOR_ID_AMD,
			      PCI_DEVICE_ID_AMD_CS5536_ISA, 0);
	if (!dev) {
		printk(BIOS_ERR, "%s: Could not find the south bridge!\n",
		       __FUNCTION__);
		return;
	}
	sb = (struct southbridge_amd_cs5536_config *)dev->device_configuration;

#if 0
	if (!IsS3Resume())
	{
		struct acpiinit *aci = acpi_init_table;
		for (; aci->ioreg; aci++) {
			outl(aci->regdata, aci->ioreg);
			inl(aci->ioreg);
		}
		pm_chipset_init();
	}
#endif

	/* Set HD IRQ. */
	outl(GPIOL_2_SET, GPIO_IO_BASE + GPIOL_INPUT_ENABLE);
	outl(GPIOL_2_SET, GPIO_IO_BASE + GPIOL_IN_AUX1_SELECT);

	/* Allow I/O reads and writes during a ATA DMA operation. This could
	 * be done in the HD ROM but do it here for easier debugging.
	 */
	msrnum = ATA_SB_GLD_MSR_ERR;
	msr = rdmsr(msrnum);
	msr.lo &= ~0x100;
	wrmsr(msrnum, msr);

	/* Enable post primary IDE. */
	msrnum = GLPCI_SB_CTRL;
	msr = rdmsr(msrnum);
	msr.lo |= GLPCI_CRTL_PPIDE_SET;
	wrmsr(msrnum, msr);

	csi = SB_MASTER_CONF_TABLE;
	for (/* Nothing */; csi->msrnum; csi++) {
		msr.lo = csi->msr.lo;
		msr.hi = csi->msr.hi;
		wrmsr(csi->msrnum, msr);	/* MSR - see table above */
	}

	/* Flash BAR size setup. */
	printk(BIOS_ERR, "%sDoing chipset_flash_setup()\n",
	       sb->enable_ide_nand_flash == 1 ? "" : "Not ");
	if (sb->enable_ide_nand_flash == 1)
		chipset_flash_setup();

	/* Set up hardware clock gating. */
	/* TODO: Why the extra block here? Can it be removed? */
	{
		csi = CS5536_CLOCK_GATING_TABLE;
		for (/* Nothing */; csi->msrnum; csi++) {
			msr.lo = csi->msr.lo;
			msr.hi = csi->msr.hi;
			wrmsr(csi->msrnum, msr); /* MSR - see table above */
		}
	}
}

/**
 * TODO.
 *
 * @param dev The device to use.
 */
static void southbridge_init(struct device *dev)
{
	struct southbridge_amd_cs5536_config *sb =
	    (struct southbridge_amd_cs5536_config *)dev->device_configuration;

	/*
	 * struct device *gpiodev;
	 * unsigned short gpiobase = MDD_GPIO;
	 */

	printk(BIOS_ERR, "cs5536: %s\n", __FUNCTION__);

	setup_i8259();
	lpc_init(sb);
	uarts_init(sb);

	if (sb->enable_gpio_int_route) {
		vr_write((VRC_MISCELLANEOUS << 8) + PCI_INT_AB,
			 (sb->enable_gpio_int_route & 0xFFFF));
		vr_write((VRC_MISCELLANEOUS << 8) + PCI_INT_CD,
			 (sb->enable_gpio_int_route >> 16));
	}

	printk(BIOS_ERR, "cs5536: %s: enable_ide_nand_flash is %d\n",
	       __FUNCTION__, sb->enable_ide_nand_flash);
	if (sb->enable_ide_nand_flash == 1) {
		enable_ide_nand_flash_header();
	}

	enable_USB_port4(sb);

#warning Add back in unwanted VPCI support
#if 0
	/* Disable unwanted virtual PCI devices. */
	for (i = 0; (i < MAX_UNWANTED_VPCI) && (0 != sb->unwanted_vpci[i]); i++) {
		printk(BIOS_DEBUG, "Disabling VPCI device: 0x%08X\n",
		       sb->unwanted_vpci[i]);
		outl(sb->unwanted_vpci[i] + 0x7C, 0xCF8);
		outl(0xDEADBEEF, 0xCFC);
	}
#endif
}

static void southbridge_enable(struct device *dev)
{
	printk(BIOS_ERR, "cs5536: Enter %s: dev is %p\n", __FUNCTION__, dev);
	printk(BIOS_ERR, "cs5536: Exit %s: dev is %p\n", __FUNCTION__, dev);
}

/**
 * A slightly different enable resources than the standard.
 * We grab control here as VSA has played in this chip as well.
 *
 * @param dev The device to use.
 */
static void cs5536_pci_dev_enable_resources(struct device *dev)
{
	/* TODO: Shouldn't this be BIOS_SPEW? */
	printk(BIOS_ERR, "cs5536: %s()\n", __FUNCTION__);
	pci_dev_enable_resources(dev);
	enable_childrens_resources(dev);
}

static struct device_operations southbridge_ops = {
	.phase3_scan			= scan_static_bus,
	.phase4_read_resources		= pci_dev_read_resources,
	.phase4_set_resources		= pci_dev_set_resources,
	.phase5_enable_resources	= cs5536_pci_dev_enable_resources,
	.phase6_init			= southbridge_init,
};

struct constructor cs5536_constructors[] = {
	{.id = {.type = DEVICE_ID_PCI,
		.u = {.pci = {.vendor = PCI_VENDOR_ID_AMD,
			      .device = PCI_DEVICE_ID_AMD_CS5536_ISA}}},
	 .ops = &southbridge_ops},

	{.ops = 0},
};
