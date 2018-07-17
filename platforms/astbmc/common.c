/* Copyright 2013-2016 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <skiboot.h>
#include <device.h>
#include <console.h>
#include <psi.h>
#include <chip.h>
#include <xscom.h>
#include <ast.h>
#include <ipmi.h>
#include <bt.h>
#include <errorlog.h>
#include <lpc.h>

#include "astbmc.h"

/* UART1 config */
#define UART_IO_BASE	0x3f8
#define UART_IO_COUNT	8
#define UART_LPC_IRQ	4

/* BT config */
#define BT_IO_BASE	0xe4
#define BT_IO_COUNT	3
#define BT_LPC_IRQ	10

/* MBOX config */
#define MBOX_IO_BASE 0x1000
#define MBOX_IO_COUNT 6
#define MBOX_LPC_IRQ 9

void astbmc_ext_irq_serirq_cpld(unsigned int chip_id)
{
	lpc_all_interrupts(chip_id);
}

static void astbmc_ipmi_error(struct ipmi_msg *msg)
{
        prlog(PR_DEBUG, "ASTBMC: error sending msg. cc = %02x\n", msg->cc);

        ipmi_free_msg(msg);
}

static void astbmc_ipmi_setenables(void)
{
        struct ipmi_msg *msg;

        struct {
                uint8_t oem2_en : 1;
                uint8_t oem1_en : 1;
                uint8_t oem0_en : 1;
                uint8_t reserved : 1;
                uint8_t sel_en : 1;
                uint8_t msgbuf_en : 1;
                uint8_t msgbuf_full_int_en : 1;
                uint8_t rxmsg_queue_int_en : 1;
        } data;

        memset(&data, 0, sizeof(data));

        /* The spec says we need to read-modify-write to not clobber
         * the state of the other flags. These are set on by the bmc */
        data.rxmsg_queue_int_en = 1;
        data.sel_en = 1;

        /* These are the ones we want to set on */
        data.msgbuf_en = 1;

        msg = ipmi_mkmsg_simple(IPMI_SET_ENABLES, &data, sizeof(data));
        if (!msg) {
		/**
		 * @fwts-label ASTBMCFailedSetEnables
		 * @fwts-advice AST BMC is likely to be non-functional
		 * when accessed from host.
		 */
                prlog(PR_ERR, "ASTBMC: failed to set enables\n");
                return;
        }

        msg->error = astbmc_ipmi_error;

        ipmi_queue_msg(msg);

}

static int astbmc_fru_init(void)
{
	const struct dt_property *prop;
	struct dt_node *node;
	uint8_t fru_id;

	node = dt_find_by_path(dt_root, "bmc");
	if (!node)
		return -1;

	prop = dt_find_property(node, "firmware-fru-id");
	if (!prop)
		return -1;

	fru_id = dt_property_get_cell(prop, 0) & 0xff;
	ipmi_fru_init(fru_id);
	return 0;
}


void astbmc_init(void)
{
	/* Initialize PNOR/NVRAM */
	pnor_init();

	/* Register the BT interface with the IPMI layer */
	bt_init();
	/* Initialize elog */
	elog_init();
	ipmi_sel_init();
	ipmi_wdt_init();
	ipmi_rtc_init();
	ipmi_opal_init();
	astbmc_fru_init();
	ipmi_sensor_init();

	/* Preload PNOR VERSION section */
	flash_fw_version_preload();

	/* Request BMC information */
	ipmi_get_bmc_info_request();

	/* As soon as IPMI is up, inform BMC we are in "S0" */
	ipmi_set_power_state(IPMI_PWR_SYS_S0_WORKING, IPMI_PWR_NOCHANGE);

        /* Enable IPMI OEM message interrupts */
        astbmc_ipmi_setenables();

	ipmi_set_fw_progress_sensor(IPMI_FW_MOTHERBOARD_INIT);

	/* Setup UART console for use by Linux via OPAL API */
	set_opal_console(&uart_opal_con);

	/* Add ibm,firmware-versions node */
	flash_dt_add_fw_version();

	/* Add BMC firmware info to device tree */
	ipmi_dt_add_bmc_info();
}

int64_t astbmc_ipmi_power_down(uint64_t request)
{
	if (request != IPMI_CHASSIS_PWR_DOWN) {
		prlog(PR_WARNING, "PLAT: unexpected shutdown request %llx\n",
				   request);
	}

	return ipmi_chassis_control(request);
}

int64_t astbmc_ipmi_reboot(void)
{
	return ipmi_chassis_control(IPMI_CHASSIS_HARD_RESET);
}

static void astbmc_fixup_dt_system_id(void)
{
	/* Make sure we don't already have one */
	if (dt_find_property(dt_root, "system-id"))
		return;

	dt_add_property_strings(dt_root, "system-id", "unavailable");
}

static void astbmc_fixup_dt_bt(struct dt_node *lpc)
{
	struct dt_node *bt;
	char namebuf[32];

	/* First check if the BT interface is already there */
	dt_for_each_child(lpc, bt) {
		if (dt_node_is_compatible(bt, "bt"))
			return;
	}

	snprintf(namebuf, sizeof(namebuf), "ipmi-bt@i%x", BT_IO_BASE);
	bt = dt_new(lpc, namebuf);

	dt_add_property_cells(bt, "reg",
			      1, /* IO space */
			      BT_IO_BASE, BT_IO_COUNT);
	dt_add_property_strings(bt, "compatible", "ipmi-bt");

	/* Mark it as reserved to avoid Linux trying to claim it */
	dt_add_property_strings(bt, "status", "reserved");

	dt_add_property_cells(bt, "interrupts", BT_LPC_IRQ);
	dt_add_property_cells(bt, "interrupt-parent", lpc->phandle);
}

static void astbmc_fixup_dt_mbox(struct dt_node *lpc)
{
	struct dt_node *mbox;
	char namebuf[32];

	/* All P9 machines use mbox. P8 machines can indicate they support
	 * it using the scratch register */
	if (proc_gen != proc_gen_p9 && !ast_scratch_reg_is_mbox())
		return;

	/* First check if the mbox interface is already there */
	dt_for_each_child(lpc, mbox) {
		if (dt_node_is_compatible(mbox, "mbox"))
			return;
	}

	snprintf(namebuf, sizeof(namebuf), "mbox@i%x", MBOX_IO_BASE);
	mbox = dt_new(lpc, namebuf);

	dt_add_property_cells(mbox, "reg",
			      1, /* IO space */
			      MBOX_IO_BASE, MBOX_IO_COUNT);
	dt_add_property_strings(mbox, "compatible", "mbox");

	/* Mark it as reserved to avoid Linux trying to claim it */
	dt_add_property_strings(mbox, "status", "reserved");

	dt_add_property_cells(mbox, "interrupts", MBOX_LPC_IRQ);
	dt_add_property_cells(mbox, "interrupt-parent", lpc->phandle);
}

static void astbmc_fixup_dt_uart(struct dt_node *lpc)
{
	/*
	 * The official OF ISA/LPC binding is a bit odd, it prefixes
	 * the unit address for IO with "i". It uses 2 cells, the first
	 * one indicating IO vs. Memory space (along with bits to
	 * represent aliasing).
	 *
	 * We pickup that binding and add to it "2" as a indication
	 * of FW space.
	 */
	struct dt_node *uart;
	char namebuf[32];

	/* First check if the UART is already there */
	dt_for_each_child(lpc, uart) {
		if (dt_node_is_compatible(uart, "ns16550"))
			return;
	}

	/* Otherwise, add a node for it */
	snprintf(namebuf, sizeof(namebuf), "serial@i%x", UART_IO_BASE);
	uart = dt_new(lpc, namebuf);

	dt_add_property_cells(uart, "reg",
			      1, /* IO space */
			      UART_IO_BASE, UART_IO_COUNT);
	dt_add_property_strings(uart, "compatible",
				"ns16550",
				"pnpPNP,501");
	dt_add_property_cells(uart, "clock-frequency", 1843200);
	dt_add_property_cells(uart, "current-speed", 115200);

	/*
	 * This is needed by Linux for some obscure reasons,
	 * we'll eventually need to sanitize it but in the meantime
	 * let's make sure it's there
	 */
	dt_add_property_strings(uart, "device_type", "serial");

	/* Add interrupt */
	dt_add_property_cells(uart, "interrupts", UART_LPC_IRQ);
	dt_add_property_cells(uart, "interrupt-parent", lpc->phandle);
}

static void del_compatible(struct dt_node *node)
{
	struct dt_property *prop;

	prop = __dt_find_property(node, "compatible");
	if (prop)
		dt_del_property(node, prop);
}


static void astbmc_fixup_bmc_sensors(void)
{
	struct dt_node *parent, *node;

	parent = dt_find_by_path(dt_root, "bmc");
	if (!parent)
		return;
	del_compatible(parent);

	parent = dt_find_by_name(parent, "sensors");
	if (!parent)
		return;
	del_compatible(parent);

	dt_for_each_child(parent, node) {
		if (dt_find_property(node, "compatible"))
			continue;
		dt_add_property_string(node, "compatible", "ibm,ipmi-sensor");
	}
}

static void astbmc_fixup_dt(void)
{
	struct dt_node *n, *primary_lpc = NULL;

	/* Find the primary LPC bus */
	dt_for_each_compatible(dt_root, n, "ibm,power8-lpc") {
		if (!primary_lpc || dt_has_node_property(n, "primary", NULL))
			primary_lpc = n;
		if (dt_has_node_property(n, "#address-cells", NULL))
			break;
	}
	dt_for_each_compatible(dt_root, n, "ibm,power9-lpc") {
		if (!primary_lpc || dt_has_node_property(n, "primary", NULL))
			primary_lpc = n;
		if (dt_has_node_property(n, "#address-cells", NULL))
			break;
	}

	if (!primary_lpc)
		return;

	/* Fixup the UART, that might be missing from HB */
	astbmc_fixup_dt_uart(primary_lpc);

	/* BT is not in HB either */
	astbmc_fixup_dt_bt(primary_lpc);

	/* MBOX is not in HB */
	astbmc_fixup_dt_mbox(primary_lpc);

	/* The pel logging code needs a system-id property to work so
	   make sure we have one. */
	astbmc_fixup_dt_system_id();

	astbmc_fixup_bmc_sensors();
}

static void astbmc_fixup_psi_bar(void)
{
	struct proc_chip *chip = next_chip(NULL);
	uint64_t psibar;

	/* This is P8 specific */
	if (proc_gen != proc_gen_p8)
		return;

	/* Read PSI BAR */
	if (xscom_read(chip->id, 0x201090A, &psibar)) {
		prerror("PLAT: Error reading PSI BAR\n");
		return;
	}
	/* Already configured, bail out */
	if (psibar & 1)
		return;

	/* Hard wire ... yuck */
	psibar = 0x3fffe80000001;

	printf("PLAT: Fixing up PSI BAR on chip %d BAR=%llx\n",
	       chip->id, psibar);

	/* Now write it */
	xscom_write(chip->id, 0x201090A, psibar);
}

static void astbmc_fixup_uart(void)
{
	/*
	 * Depending on which image we are running, it may be configuring the
	 * virtual UART or not.  Check if VUART is enabled and use SIO if not.
	 * We also correct the configuration of VUART as some BMC images don't
	 * setup the interrupt properly
	 */
	if (ast_is_vuart1_enabled()) {
		printf("PLAT: Using virtual UART\n");
		ast_disable_sio_uart1();
		ast_setup_vuart1(UART_IO_BASE, UART_LPC_IRQ);
	} else {
		printf("PLAT: Using SuperIO UART\n");
		ast_setup_sio_uart1(UART_IO_BASE, UART_LPC_IRQ);
	}
}

void astbmc_early_init(void)
{
	/* Hostboot's device-tree isn't quite right yet */
	astbmc_fixup_dt();

	/* Hostboot forgets to populate the PSI BAR */
	astbmc_fixup_psi_bar();

	/* Send external interrupts to me */
	psi_set_external_irq_policy(EXTERNAL_IRQ_POLICY_SKIBOOT);

	if (ast_sio_init()) {
		if (!ast_can_isolate_sp()) {
			/*
			 * BMCs claiming support for isolation must have
			 * correctly configured the UART and BT for host
			 * firmware. If not, let's apply some fixups for broken
			 * BMC firmwares.
			 */
			if (ast_io_init()) {
				astbmc_fixup_uart();
				ast_setup_ibt(BT_IO_BASE, BT_LPC_IRQ);
			} else
				prerror("PLAT: AST IO initialisation failed!\n");
		}

		ast_setup_sio_mbox(MBOX_IO_BASE, MBOX_LPC_IRQ);
	} else
		prerror("PLAT: AST SIO initialisation failed!\n");

	/* Setup UART and use it as console */
	uart_init();

	mbox_init();

	prd_init();
}

static bool astbmc_isolate_via_io(void)
{
	return ast_sio_disable();
}

static bool astbmc_isolate_via_ipmi(void)
{
	return false;
}

static void astbmc_isolate(void)
{
	bool isolated;

	isolated = ast_io_is_rw() ? astbmc_isolate_via_io()
				  : astbmc_isolate_via_ipmi();

	if (!isolated) {
		prlog(PR_EMERG, "PLAT: BMC isolation failed\n");
		abort();
	}

	prlog(PR_INFO, "PLAT: Isolated BMC\n");
}

void astbmc_exit(void)
{
	if (ast_can_isolate_sp())
		astbmc_isolate();
	ipmi_wdt_final_reset();
}

const struct bmc_platform astbmc_ami = {
	.name = "AMI",
	.ipmi_oem_partial_add_esel   = IPMI_CODE(0x3a, 0xf0),
	.ipmi_oem_pnor_access_status = IPMI_CODE(0x3a, 0x07),
};

const struct bmc_platform astbmc_openbmc = {
	.name = "OpenBMC",
	.ipmi_oem_partial_add_esel   = IPMI_CODE(0x3a, 0xf0),
};
