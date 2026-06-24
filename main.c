/*
 * Epic! - a full-system emulator for Intel IA64
 * main.c - Entry point, argument parsing, machine init (iirc)
 *
 * MIT License
 *
 * Copyright (c) 2025 - 2026 epic64
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "common.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "cpu/exec_fp.h"
#include "cpu/tlb.h"
#include "memory/memory.h"
#include "ia64/ivt.h"
#include "devices/sapic.h"
#include "devices/pci.h"
#include "devices/ide.h"
#include "devices/uart.h"
#include "devices/rtc.h"
#include "devices/ps2.h"
#include "devices/boot.h"
#include "fw/pal.h"
#include "fw/sal.h"
#include "fw/acpi.h"
#include "fw/efi.h"

#include <signal.h>

/* interruption vector table sits at this offset inside the firmware rom */
#define FIRMWARE_IVT_OFFSET 0x8000

static IA64CPU *g_running_cpu = NULL;
static void sigint_handler(int sig) {
    (void)sig;
    if (g_running_cpu) {
        fprintf(stderr, "\nCaught Ctrl+C — stopping CPU...\n");
        g_running_cpu->running = false;
    }
}

/*mach structure*/

typedef struct IA64Machine {
    const char     *name;
    MemorySubsystem *memory;
    IA64CPU        *cpus[4];    /* allows for up to 4 vCPUs (not needed now tho) */
    int             num_cpus;
    uint64_t        ram_size;

    /* config */
    const char     *cdrom_path;
    const char     *fda_path;
    const char     *hda_path;
    const char     *hdb_path;
    const char     *firmware_path;
    int             serial_mode;    /* 0=stdio, 1=file, 2=none */
    const char     *serial_file;    /* serial_mode=1 */
    int             display_mode;   /* 0=none (terminal), 1=sdl */
    bool            acpi_enabled;
    int             max_instructions; /* 0 = unlimited */

    /* devices */
    LocalSAPIC      local_sapic[4];  /* per-CPU interrupt controller */
    IoSAPIC         io_sapic;       /* IO interrupt router */
    PciBus          pci_bus;        /* pci bus subsystem */
    IDEController   ide[2];         /* IDE primary + secondary */
    UART16550       uart[2];        /* COM1 + COM2 */
    RTC             rtc;            /* RTC */
    PS2Controller   ps2;            /* PS/2 keyboard */
    BootDevice      boot_dev[4];    /* Boot devs */
    int             num_boot_devs;
} IA64Machine;

/* glob mach pointer (this is used by sal.c for pci bus access) */
static IA64Machine *g_machine = NULL;

/* once again called by sal.c to get the pci bus */
PciBus *sal_get_pci_bus(void) {
    return g_machine ? &g_machine->pci_bus : NULL;
}

/* called by the EFI code to access devs */
UART16550 *efi_get_uart(int port) {
    if (!g_machine || port < 0 || port > 1) return NULL;
    return &g_machine->uart[port];
}

RTC *efi_get_rtc(void) {
    return g_machine ? &g_machine->rtc : NULL;
}

MemorySubsystem *efi_get_memory(void) {
    return g_machine ? g_machine->memory : NULL;
}

IA64CPU *efi_get_cpu(int index) {
    if (!g_machine || index < 0 || index >= g_machine->num_cpus) return NULL;
    return g_machine->cpus[index];
}

IDEDevice *efi_get_ide_device(int channel, int drive) {
    if (!g_machine || channel < 0 || channel > 1) return NULL;
    if (drive < 0 || drive > 1) return NULL;
    return &g_machine->ide[channel].devices[drive];
}

/* Called by sapic_io.c to get a CPU's Local SAPIC */
LocalSAPIC *machine_get_local_sapic(IA64Machine *m, int cpu) {
    if (!m || cpu < 0 || cpu >= m->num_cpus) return NULL;
    return &m->local_sapic[cpu];
}

/* CPU external interrupt check callback — checks the local SAPIC */
static uint8_t cpu_check_ext_int_cb(IA64CPU *cpu) {
    IA64Machine *m = (IA64Machine *)cpu->machine;
    if (!m) return 0;
    return local_sapic_check_pending(&m->local_sapic[cpu->cpu_id]);
}

/*  Firmware init */

/* pretty much what this section does is installs a minimal fw into the fw ROM region & initializes the fw subsystem (pal, sal, acpi, etc...) */
static void firmware_init(IA64Machine *machine) {
    if (!machine->memory || !machine->memory->firmware_rom) return;

    uint8_t *rom = machine->memory->firmware_rom;
    int offset = 0;

    /* bundle 0 - MII temp., all nops */
    {
        uint64_t lo = 0x0000000000000000ULL;
        uint64_t hi = 0x0000000000000000ULL;
        memcpy(rom + offset, &lo, 8); offset += 8;
        memcpy(rom + offset, &hi, 8); offset += 8;
    }

    /* bundle 1: MLX temp., nop and break */
    {
        uint64_t lo = 0x0000000000000002ULL;
        uint64_t hi = 0x0000000000000000ULL;
        memcpy(rom + offset, &lo, 8); offset += 8;
        memcpy(rom + offset, &hi, 8); offset += 8;
    }

    /* this will fill rest of the rom with nop bundles */
    while (offset + 16 <= (int)PHYS_FIRMWARE_ROM_SIZE) {
        uint64_t lo = 0;
        uint64_t hi = 0;
        memcpy(rom + offset, &lo, 8); offset += 8;
        memcpy(rom + offset, &hi, 8); offset += 8;
    }

    LOG_I("Firmware ROM initialized (%llu KB)",
          (unsigned long long)(PHYS_FIRMWARE_ROM_SIZE / 1024));

    /* firmware subsystem init */

    /* init PAL procedures */
    pal_init();

    /* init SAL (builds system table in mem) */
    sal_init((struct IA64Machine *)machine);

    /* gen ACPI tables */
    if (machine->acpi_enabled) {
        acpi_generate_tables((struct IA64Machine *)machine,
                              machine->num_cpus);
    }

    /* init EFI tables & function bundles */
    efi_init_tables(machine->memory, machine->ram_size);
    {
        uint8_t *ivt = rom + FIRMWARE_IVT_OFFSET;
        /* bbb template (0x0c): slot0=rfi, slot1=nop.b, slot2=nop.b */
        uint64_t rfi_lo = 0x000068080000002CULL;
        uint64_t rfi_hi = 0x2000000000900000ULL;
        static const uint16_t vecs[] = {
            IVT_VHPT_HASH, IVT_MCA,
            IVT_ITLB, IVT_DTLB, IVT_ALT_ITLB, IVT_ALT_DTLB,
            IVT_DATA_NESTED_TLB, IVT_INST_KEY_MISS, IVT_DATA_KEY_MISS,
            IVT_DIRTY_BIT, IVT_INST_ACCESS_BIT, IVT_DATA_ACCESS_BIT,
            IVT_BREAK_INST, IVT_EXT_INT,
            IVT_PAGE_NOT_PRESENT, IVT_INST_KEY_PERM, IVT_DATA_KEY_PERM,
            IVT_GEN_EXCEPTION, IVT_DISABLED_FP_REG, IVT_NAT_CONSUMPTION,
            IVT_SPECULATION, IVT_DEBUG_EXCEPTION, IVT_UNALIGNED_REF,
            IVT_UNSUPPORTED_DATA_REF,
            IVT_FP_FAULT, IVT_FP_TRAP,
            IVT_LOWER_PRIV_TRANSFER, IVT_TAKEN_BRANCH, IVT_SINGLE_STEP,
            IVT_IA32_EXCEPTION, IVT_IA32_INTERCEPT, IVT_IA32_INTERRUPT,
        };
        for (size_t i = 0; i < sizeof(vecs)/sizeof(vecs[0]); i++) {
            memcpy(ivt + vecs[i], &rfi_lo, 8);
            memcpy(ivt + vecs[i] + 8, &rfi_hi, 8);
        }
        LOG_I("IVT installed at 0x%llX (%zu vectors)",
              (unsigned long long)(PHYS_FIRMWARE_ROM_BASE + FIRMWARE_IVT_OFFSET),
              sizeof(vecs)/sizeof(vecs[0]));
    }

    /* print banner to UART out */
    LOG_I("Epic!, an Itanium emulator :3 v" EPIC_VERSION);
    LOG_I("Debug FW: PAL 3.1, SAL 3.1, ACPI 2.0");
}


/*  mach init */

/* nul MMIO callbacks (for HPET placeholder?) */
static uint64_t null_read(void *opaque, uint64_t addr, unsigned size) {
    (void)opaque; (void)addr; (void)size; return 0;
}
static void null_write(void *opaque, uint64_t addr,
                        uint64_t value, unsigned size) {
    (void)opaque; (void)addr; (void)value; (void)size;
}

/*  mmio callback wrappers for real devics*/

static MemOps io_sapic_mmio_ops = {
    .read  = io_sapic_mmio_read,
    .write = io_sapic_mmio_write,
};

static MemOps local_sapic_mmio_ops = {
    .read  = local_sapic_mmio_read,
    .write = local_sapic_mmio_write,
};

static MemOps pci_mmio_ops = {
    .read  = pci_mmio_read,
    .write = pci_mmio_write,
};

static IA64Machine *machine_create(const char *name, uint64_t ram_size,
                                    int num_cpus,
                                    int serial_mode,
                                    const char *serial_file) {
    IA64Machine *machine = (IA64Machine *)calloc(1, sizeof(IA64Machine));
    if (!machine) {
        LOG_F("Failed to allocate machine");
        return NULL;
    }

    machine->name = name;
    machine->ram_size = ram_size;
    machine->num_cpus = num_cpus;
    machine->acpi_enabled = true;
    machine->serial_mode = serial_mode;
    machine->serial_file = serial_file;
    g_machine = machine;  /* set global for sal_get_pci_bus */

    /* init memory subsystem */
    machine->memory = mem_init(ram_size);
    if (!machine->memory) {
        free(machine);
        g_machine = NULL;
        return NULL;
    }

    /* devices init*/

    /* IO SAPIC */
    io_sapic_init(&machine->io_sapic, (struct IA64Machine *)machine);

    /* register MMIO regions */
    mem_register_mmio(machine->memory, PHYS_IO_SAPIC_BASE,
                      PHYS_IO_SAPIC_SIZE, &io_sapic_mmio_ops,
                      &machine->io_sapic);
    mem_register_mmio(machine->memory, PHYS_LOCAL_SAPIC_BASE,
                      PHYS_LOCAL_SAPIC_SIZE, &local_sapic_mmio_ops,
                      machine);
    mem_register_mmio(machine->memory, PHYS_PCI_MMIO_BASE,
                      PHYS_PCI_MMIO_SIZE, &pci_mmio_ops,
                      &machine->pci_bus);

    /* HPET placeholder (null device)*/
    {
        static MemOps hpet_null_ops = { .read = null_read, .write = null_write };
        mem_register_mmio(machine->memory, PHYS_HPET_BASE,
                          PHYS_HPET_SIZE, &hpet_null_ops, NULL);
    }

    /* UART init */
    UartBackend backend = (machine->serial_mode == 2) ?
                           UART_BACKEND_NULL : UART_BACKEND_STDIO;
    uart_init(&machine->uart[0], 0, backend,
              machine->serial_file, &machine->io_sapic);
    uart_init(&machine->uart[1], 1, UART_BACKEND_NULL,
              NULL, &machine->io_sapic);

    /* RTC */
    rtc_init(&machine->rtc, &machine->io_sapic);

    /* PS2 */
    ps2_init(&machine->ps2, &machine->io_sapic);

    /* pci bus */
    pci_bus_init(&machine->pci_bus, (struct IA64Machine *)machine);

    /* ide :regional_indicator_e: controllers AAAAAAAAAAAss*/
    ide_init(&machine->ide[0], 0, NULL, NULL, &machine->io_sapic);
    ide_init(&machine->ide[1], 1, NULL, NULL, &machine->io_sapic);

    /* register FW ram region for ACPI/SAL/DSDT tablse
     * covers 0xC0000-0xFFFFF (256KB), option ROM slash BIOS data area */
    mem_register_ram(machine->memory, PHYS_ROM_SHADOW_BASE,
                     PHYS_ROM_SHADOW_SIZE);

    /* register high memory region (4GB+) for kernel access */
    /* linux IA-64 kernel uses memory above 4GB for various data structures */
    mem_register_ram(machine->memory, 0x100000000ULL, 0xF00000000ULL);  /* 60GB at 4GB (up to 64GB) */

    firmware_init(machine);

    /* create cpus */
    for (int i = 0; i < num_cpus; i++) {
        machine->cpus[i] = cpu_create(i, (struct IA64Machine *)machine);
        if (!machine->cpus[i]) {
            LOG_F("Failed to create CPU %d", i);
            for (int j = 0; j < i; j++) {
                cpu_destroy(machine->cpus[j]);
            }
            mem_destroy(machine->memory);
            free(machine);
            g_machine = NULL;
            return NULL;
        }
        /* init local SAPIC for this CPU wilted rose emoji */
        local_sapic_init(&machine->local_sapic[i], machine->cpus[i], i);
        /* wire SAPIC check into CPU interrupt delivery */
        machine->cpus[i]->check_ext_int = cpu_check_ext_int_cb;
    }

    LOG_I("Machine '%s' created: %d CPU(s), %llu MB RAM",
          name, num_cpus, (unsigned long long)(ram_size / (1024 * 1024)));

    /* setup EFI handoff state (CPUs must exist for this, duhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh) */
    efi_setup_handoff();

    return machine;
}

/* post-creation setup, attach dick images and probe boot devices */
static void machine_attach_storage(IA64Machine *machine) {
    if (!machine) return;

    /* reinit IDE with the paths */
    ide_destroy(&machine->ide[0]);
    ide_destroy(&machine->ide[1]);
    ide_init(&machine->ide[0], 0,
             machine->hda_path, machine->cdrom_path,
             &machine->io_sapic);
    ide_init(&machine->ide[1], 1,
             machine->hdb_path, NULL,
             &machine->io_sapic);

    /* probe the boot devices */
    machine->num_boot_devs = 0;
    const char *boot_paths[] = {
        machine->hda_path, machine->cdrom_path,
        machine->hdb_path, machine->fda_path
    };
    for (int i = 0; i < 4; i++) {
        if (boot_paths[i]) {
            if (boot_device_open(&machine->boot_dev[machine->num_boot_devs],
                                  boot_paths[i]) == 0) {
                machine->num_boot_devs++;
            }
        }
    }
    LOG_I("Storage: %d boot device(s) probed", machine->num_boot_devs);
}

static void machine_destroy(IA64Machine *machine) {
    if (!machine) return;

    for (int i = 0; i < machine->num_cpus; i++) {
        if (machine->cpus[i]) {
            cpu_destroy(machine->cpus[i]);
        }
    }

    /* clean up devices */
    ide_destroy(&machine->ide[0]);
    ide_destroy(&machine->ide[1]);
    uart_destroy(&machine->uart[0]);
    uart_destroy(&machine->uart[1]);
    for (int i = 0; i < machine->num_boot_devs; i++) {
        boot_device_close(&machine->boot_dev[i]);
    }

    if (machine->memory) {
        mem_dump_map(machine->memory);
        mem_destroy(machine->memory);
    }

    g_machine = NULL;
    free(machine);
    LOG_I("Machine destroyed");
}

/*test for CPU + mem + bundle decoding*/

static void run_basic_cpu_tests(IA64Machine *machine) {
    IA64CPU *cpu = machine->cpus[0];

    LOG_I("basic cpu validation beginning NOW1");

    /* #1 verif CPU reset state */
    LOG_I("test 1: CPU reset state");
    if (cpu->psr == 0) {
        LOG_I("  PASS: PSR = 0");
    } else {
        LOG_E("  FAIL: PSR = 0x%016llX (expected 0)", (unsigned long long)cpu->psr);
    }
    if (cpu->cpl == 0) {
        LOG_I("  PASS: CPL = 0");
    } else {
        LOG_E("  FAIL: CPL = %d (expected 0)", cpu->cpl);
    }
    if (!PSR_DT(cpu->psr) && !PSR_IT(cpu->psr)) {
        LOG_I("  PASS: Translation disabled (physical mode)");
    } else {
        LOG_E("  FAIL: Translation should be disabled at reset");
    }
    if (cpu->pr[0] == true) {
        LOG_I("  PASS: PR0 = TRUE");
    } else {
        LOG_E("  FAIL: PR0 should be TRUE");
    }

    /* #2 verif PSR field manipulation */
    LOG_I("test 1: PSR field manip");
    uint64_t test_psr = 0;
    test_psr = psr_set(test_psr, PSR_DT_MASK);
    if (PSR_DT(test_psr)) {
        LOG_I("  PASS: PSR.dt set correctly");
    } else {
        LOG_E("  FAIL: PSR.dt not set");
    }
    test_psr = psr_set_cpl(test_psr, 3);
    if (PSR_CPL(test_psr) == 3) {
        LOG_I("  PASS: PSR.cpl = 3");
    } else {
        LOG_E("  FAIL: PSR.cpl = %d (expected 3)", (int)PSR_CPL(test_psr));
    }
    test_psr = psr_on_interruption(test_psr);
    if (!PSR_IC(test_psr) && !PSR_I(test_psr) &&
        !PSR_DT(test_psr) && !PSR_IT(test_psr) && PSR_RT(test_psr) &&
        PSR_CPL(test_psr) == 0 && PSR_BN(test_psr)) {
        LOG_I("  PASS: psr_on_interruption() correct");
    } else {
        LOG_E("  FAIL: psr_on_interruption() = 0x%016llX",
              (unsigned long long)test_psr);
    }

    /* 3 verif memory subsystem */
    LOG_I("--- Test 3: Memory subsystem ---");
    int fault = 0;

    /* r&w back from main RAM */
    phys_mem_write(cpu, PHYS_MAIN_RAM_BASE, 0xDEADBEEFCAFEBABEULL, 8, &fault);
    uint64_t val = phys_mem_read(cpu, PHYS_MAIN_RAM_BASE, 8, &fault);
    if (val == 0xDEADBEEFCAFEBABEULL && !fault) {
        LOG_I("  PASS: Main RAM read/write (64-bit)");
    } else {
        LOG_E("  FAIL: Main RAM: read 0x%016llX, fault=%d",
              (unsigned long long)val, fault);
    }

    /* r&w back from low memory */
    phys_mem_write(cpu, 0x1000, 0x12345678, 4, &fault);
    val = phys_mem_read(cpu, 0x1000, 4, &fault);
    if (val == 0x12345678 && !fault) {
        LOG_I("  PASS: Low memory read/write (32-bit)");
    } else {
        LOG_E("  FAIL: Low memory: read 0x%08llX, fault=%d",
              (unsigned long long)val, fault);
    }

    /* block rw test */
    uint8_t pattern[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t readback[16] = {0};
    phys_mem_block_write(PHYS_MAIN_RAM_BASE + 0x10000, pattern, 16);
    phys_mem_block_read(PHYS_MAIN_RAM_BASE + 0x10000, readback, 16);
    if (memcmp(pattern, readback, 16) == 0) {
        LOG_I("  PASS: Block read/write (16 bytes)");
    } else {
        LOG_E("  FAIL: Block read/write mismatch");
    }

    /* MMIO read (this is a placeholder null device aka a stub because i'm lazy) */
    val = phys_mem_read(cpu, PHYS_IO_SAPIC_BASE, 4, &fault);
    if (!fault) {
        LOG_I("  PASS: MMIO read (null device) returned 0x%llX",
              (unsigned long long)val);
    } else {
        LOG_E("  FAIL: MMIO read faulted");
    }

    /* 4 verif bundle decode */
    LOG_I("--- Test 4: Bundle decode ---");

    /* write a NOP bundle to RAM and TRY to decode it */
    Bundle nop_bundle = { .lo = 0x0000000000000000ULL,
                          .hi = 0x0000000000000000ULL };
    phys_mem_block_write(PHYS_MAIN_RAM_BASE, &nop_bundle, 16);

    /* set CPU IP to the bundle location (phys mode) */
    cpu->ip = make_ip(PHYS_MAIN_RAM_BASE, 0);

    /* try to fetch and decode */
    DecodedBundle decoded;
    /* read bundle directly for test */
    Bundle test_bundle;
    phys_mem_block_read(PHYS_MAIN_RAM_BASE, &test_bundle, 16);

    if (bundle_decode(test_bundle, &decoded)) {
        LOG_I("  PASS: Decoded MII template (id=%d, name=%s)",
              decoded.template_id,
              TEMPLATE_TABLE[decoded.template_id].name);
        LOG_I("        Slot types: %d/%d/%d",
              decoded.slots[0].type, decoded.slots[1].type,
              decoded.slots[2].type);
        if (decoded.slots[0].type == SLOT_M &&
            decoded.slots[1].type == SLOT_I &&
            decoded.slots[2].type == SLOT_I) {
            LOG_I("  PASS: Slot types correct (M, I, I)");
        } else {
            LOG_E("  FAIL: Expected M/I/I, got %d/%d/%d",
                  decoded.slots[0].type, decoded.slots[1].type,
                  decoded.slots[2].type);
        }
    } else {
        LOG_E("  FAIL: Could not decode template 0x00");
    }

    /* test MLX template */
    Bundle mlx_bundle = { .lo = 0x0000000000000002ULL,
                          .hi = 0x0000000000000000ULL };
    if (bundle_decode(mlx_bundle, &decoded)) {
        LOG_I("  PASS: Decoded MLX template (id=%d, name=%s)",
              decoded.template_id,
              TEMPLATE_TABLE[decoded.template_id].name);
        if (decoded.slots[0].type == SLOT_M &&
            decoded.slots[1].type == SLOT_L &&
            decoded.slots[2].type == SLOT_X) {
            LOG_I("  PASS: Slot types correct (M, L, X)");
        } else {
            LOG_E("  FAIL: Expected M/L/X");
        }
    } else {
        LOG_E("  FAIL: Could not decode template 0x02");
    }

    /* test mmi; template (0x0A) */
    Bundle mmi_bundle = { .lo = 0x000000000000000AULL,
                           .hi = 0x0000000000000000ULL };
    if (bundle_decode(mmi_bundle, &decoded) &&
        decoded.template_id == 0x0A &&
        decoded.slots[0].type == SLOT_M &&
        decoded.slots[1].type == SLOT_M &&
        decoded.slots[2].type == SLOT_I &&
        decoded.slots[2].stop) {
        LOG_I("  PASS: mmi; template (0x0A) decoded correctly");
    } else {
        LOG_E("  FAIL: mmi; template (0x0A) decode failed");
    }

    /* 5 verif RSE basics */
    LOG_I("--- Test 5: RSE basics ---");
    uint64_t cfm = rse_make_cfm(cpu);
    if (cfm & IFS_V) {
        LOG_I("  PASS: CFM valid bit set");
    } else {
        LOG_E("  FAIL: CFM valid bit not set");
    }

    /* test BSP adjustment */
    int nat_collected = 0;
    uint64_t bsp = 0x1000;
    uint64_t new_bsp = rse_bsp_adjust(bsp, 3, &nat_collected);
    if (new_bsp > bsp && new_bsp == bsp + 24) {
        LOG_I("  PASS: BSP adjust +3 (no NaT points)");
    } else {
        LOG_I("  INFO: BSP adjust +3: 0x%llX -> 0x%llX (nat=%d)",
              (unsigned long long)bsp, (unsigned long long)new_bsp,
              nat_collected);
    }

    /* 6 verif interruption delivery */
    LOG_I("--- Test 6: Interruption delivery ---");
    cpu->ip = make_ip(PHYS_MAIN_RAM_BASE + 0x100, 0);
    cpu->psr = psr_set(psr_set_cpl(0, 3), PSR_I_MASK);  /* CPL=3, i=1 */

    cpu_deliver_interruption(cpu, IVT_EXT_INT, ISR_ED, 0, 0, 0);

    if (PSR_CPL(cpu->psr) == 0) {
        LOG_I("  PASS: CPL promoted to 0 on interruption");
    } else {
        LOG_E("  FAIL: CPL = %d (expected 0)", (int)PSR_CPL(cpu->psr));
    }
    if (!PSR_IC(cpu->psr) && !PSR_I(cpu->psr)) {
        LOG_I("  PASS: ic=0, i=0 on interruption");
    } else {
        LOG_E("  FAIL: ic/i not cleared");
    }
    if (cpu->cr[CR_IPSR] == (psr_set(psr_set_cpl(0, 3), PSR_I_MASK))) {
        LOG_I("  PASS: IPSR saved correctly");
    } else {
        LOG_E("  FAIL: IPSR = 0x%016llX", (unsigned long long)cpu->cr[CR_IPSR]);
    }
    if ((cpu->ip & IP_BUNDLE_MASK) == (cpu->cr[CR_IVA] & IVT_ALIGN_MASK) + IVT_EXT_INT) {
        LOG_I("  PASS: IP vectored to External Interrupt handler");
    } else {
        LOG_E("  FAIL: IP = 0x%016llX (expected IVA+0x3000)",
              (unsigned long long)cpu->ip);
    }

    /* 7 test rfi */
    LOG_I("--- Test 7: rfi ---");
    cpu->cr[CR_IFS] = IFS_V;  /* set IFS valid */
    ExecStatus st = cpu_rfi(cpu);
    if (st == EX_RFI) {
        LOG_I("  PASS: rfi returned EX_RFI");
    } else {
        LOG_E("  FAIL: rfi returned %d", (int)st);
    }
    if (PSR_CPL(cpu->psr) == 3) {
        LOG_I("  PASS: CPL restored to 3 after rfi");
    } else {
        LOG_E("  FAIL: CPL = %d (expected 3)", (int)PSR_CPL(cpu->psr));
    }

    LOG_I("=== Basic CPU Tests Complete ===");
}

/* exec unit tests */

static void run_exec_tests(IA64Machine *machine) {
    IA64CPU *cpu = machine->cpus[0];
    int pass = 0, fail = 0;

    LOG_I("=== Exec Unit Tests ===");

    /* reset CPU state for clean tests */
    cpu_reset(cpu);
    cpu->ip = make_ip(PHYS_MAIN_RAM_BASE, 0);

    /* #1 integer ALU instructions */
    LOG_I("--- Test 1: Integer ALU ---");

    /* test ADD so GR4 = GR5 + GR6 */
    cpu_write_gr(cpu, 5, (GREG){100, false});
    cpu_write_gr(cpu, 6, (GREG){200, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_ADD;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5; d.r3 = 6;
        ExecStatus st = exec_integer(cpu, &d);
        GREG result = cpu_read_gr(cpu, 4);
        if (result.val == 300 && !result.nat && st == EX_SUCCESS) {
            LOG_I("  PASS: add r4=r5+r6 (100+200=300)");
            pass++;
        } else {
            LOG_E("  FAIL: add r4=r5+r6: got %llu, expected 300",
                  (unsigned long long)result.val);
            fail++;
        }
    }

    /* test not a thing (NAT) propagation */
    cpu_write_gr(cpu, 5, (GREG){42, true});  /* NaT */
    cpu_write_gr(cpu, 6, (GREG){10, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_ADD;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5; d.r3 = 6;
        ExecStatus st = exec_integer(cpu, &d);
        if (st == EX_FAULT) {
            LOG_I("  PASS: add with NaT source -> fault");
            pass++;
        } else {
            LOG_E("  FAIL: add with NaT source should fault, got %d", (int)st);
            fail++;
        }
        cpu->exception_pending = 0;  /* clear da pending fault */
    }

    /* test SUB */
    cpu_write_gr(cpu, 5, (GREG){500, false});
    cpu_write_gr(cpu, 6, (GREG){123, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_SUB;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5; d.r3 = 6;
        exec_integer(cpu, &d);
        GREG result = cpu_read_gr(cpu, 4);
        if (result.val == 377 && !result.nat) {
            LOG_I("  PASS: sub r4=r5-r6 (500-123=377)");
            pass++;
        } else {
            LOG_E("  FAIL: sub: got %llu", (unsigned long long)result.val);
            fail++;
        }
    }

    /* test AND/OR/XOR */
    cpu_write_gr(cpu, 5, (GREG){0xFF00FF00ULL, false});
    cpu_write_gr(cpu, 6, (GREG){0x0F0F0F0FULL, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_AND;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5; d.r3 = 6;
        exec_integer(cpu, &d);
        GREG result = cpu_read_gr(cpu, 4);
        if (result.val == 0x0F000F00ULL) {
            LOG_I("  PASS: and r4=r5,r6");
            pass++;
        } else {
            LOG_E("  FAIL: and: got 0x%llX", (unsigned long long)result.val);
            fail++;
        }
    }

    /* test CMP.EQ */
    cpu_write_gr(cpu, 5, (GREG){42, false});
    cpu_write_gr(cpu, 6, (GREG){42, false});
    cpu->pr[2] = false; cpu->pr[3] = false;
    {
        DecodedInst d = {0};
        d.inst_id = INST_CMP_EQ;
        d.qp = 0;
        d.p1 = 2; d.p2 = 3;
        d.r2 = 5; d.r3 = 6;
        d.ctype = CMP_NONE;
        exec_integer(cpu, &d);
        if (cpu->pr[2] == true && cpu->pr[3] == false) {
            LOG_I("  PASS: cmp.eq (42==42)");
            pass++;
        } else {
            LOG_E("  FAIL: cmp.eq: p2=%d p3=%d", cpu->pr[2], cpu->pr[3]);
            fail++;
        }
    }

    /* test SHL */
    cpu_write_gr(cpu, 5, (GREG){1, false});
    cpu_write_gr(cpu, 6, (GREG){10, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_SHL;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5; d.r3 = 6;
        exec_integer(cpu, &d);
        GREG result = cpu_read_gr(cpu, 4);
        if (result.val == 1024) {
            LOG_I("  PASS: shl r4=r5,r6 (1<<10=1024)");
            pass++;
        } else {
            LOG_E("  FAIL: shl: got %llu", (unsigned long long)result.val);
            fail++;
        }
    }

    /* test SXT4 */
    cpu_write_gr(cpu, 5, (GREG){0x80000000ULL, false});
    {
        DecodedInst d = {0};
        d.inst_id = INST_SXT4;
        d.qp = 0;
        d.r1 = 4; d.r2 = 5;
        exec_integer(cpu, &d);
        GREG result = cpu_read_gr(cpu, 4);
        if (result.val == 0xFFFFFFFF80000000ULL) {
            LOG_I("  PASS: sxt4 (sign-extend 32-bit)");
            pass++;
        } else {
            LOG_E("  FAIL: sxt4: got 0x%llX", (unsigned long long)result.val);
            fail++;
        }
    }

    /* #2 memory load/store */
    LOG_I("--- Test 2: Memory load/store ---");

    /* write a value to memory and read it back */
    {
        int fault = 0;
        phys_mem_write(cpu, PHYS_MAIN_RAM_BASE + 0x1000,
                       0xCAFEBABE12345678ULL, 8, &fault);

        /* set up base register */
        cpu_write_gr(cpu, 10, (GREG){PHYS_MAIN_RAM_BASE + 0x1000, false});

        DecodedInst d = {0};
        d.inst_id = INST_LD8;
        d.qp = 0;
        d.r1 = 8;
        d.r3 = 10;
        d.mem_size = 8;
        ExecStatus st = exec_memory(cpu, &d);
        GREG result = cpu_read_gr(cpu, 8);
        if (result.val == 0xCAFEBABE12345678ULL && !result.nat && st == EX_SUCCESS) {
            LOG_I("  PASS: ld8 r8=[r10]");
            pass++;
        } else {
            LOG_E("  FAIL: ld8: got 0x%llX, nat=%d, st=%d",
                  (unsigned long long)result.val, result.nat, (int)st);
            fail++;
        }
    }

    /* test ST4 */
    {
        cpu_write_gr(cpu, 10, (GREG){PHYS_MAIN_RAM_BASE + 0x2000, false});
        cpu_write_gr(cpu, 9, (GREG){0xDEADBEEF, false});

        DecodedInst d = {0};
        d.inst_id = INST_ST4;
        d.qp = 0;
        d.r1 = 10;  /* base address */
        d.r3 = 9;   /* value to store */
        d.mem_size = 4;
        exec_memory(cpu, &d);

        int fault = 0;
        uint64_t val = phys_mem_read(cpu, PHYS_MAIN_RAM_BASE + 0x2000, 4, &fault);
        if (val == 0xDEADBEEF && !fault) {
            LOG_I("  PASS: st4 [r10]=r9");
            pass++;
        } else {
            LOG_E("  FAIL: st4: read back 0x%llX", (unsigned long long)val);
            fail++;
        }
    }

    /* #3 TLB insertion and lookup */
    LOG_I("--- Test 3: TLB ---");

    /* insert a DTR entry; so VA 0x6000000000000000 -> PA 0x100000, 4KB page */
    cpu->rr[6] = (1ULL << 8);  /* RR6: RID=1 */
    {
        uint64_t gr_r = 0;
        gr_r |= 0x7;            /* p=1, ma=0 (WB) */
        gr_r |= (0ULL << 9);   /* ar=0 (RWX) */
        gr_r |= (0ULL << 12);  /* pl=0 */
        gr_r |= (1ULL << 15);  /* d=1 */
        gr_r |= (0x100000ULL); /* PPN = 0x100000 */

        uint64_t itir = (1ULL << 14);  /* ps=16 (bit 14 set = 2^14 = 16KB page) */
        uint64_t ifa = 0x6000000000000000ULL;

        tlb_insert_dtr(cpu, 0, gr_r, itir, ifa);

        if (cpu->dtr[0].p) {
            LOG_I("  PASS: DTR[0] inserted (p=1)");
            pass++;
        } else {
            LOG_E("  FAIL: DTR[0] not present");
            fail++;
        }
    }

    /* test access rights table */
    {
        bool ok = check_access_rights(0, 0, 0, TLB_ACCESS_READ);
        bool ok2 = check_access_rights(4, 0, 0, TLB_ACCESS_WRITE);
        bool ok3 = check_access_rights(7, 0, 0, TLB_ACCESS_READ);
        if (ok && !ok2 && !ok3) {
            LOG_I("  PASS: Access rights table correct");
            pass++;
        } else {
            LOG_E("  FAIL: Access rights: AR0/rd=%d AR4/wr=%d AR7/rd=%d",
                  ok, ok2, ok3);
            fail++;
        }
    }

    /* 4 floating point format conversions */
    LOG_I("--- Test 4: FP conversions ---");

    /* test int64 to FP to int64 roundtrip */
    {
        FREG f = int64_to_fr(12345678LL);
        int64_t back = fr_to_int64(&f);
        if (back == 12345678LL) {
            LOG_I("  PASS: int64->FP->int64 (12345678)");
            pass++;
        } else {
            LOG_E("  FAIL: int64 roundtrip: got %lld", (long long)back);
            fail++;
        }
    }

    /* test IEEE 754 conversion */
    {
        /* 1.0 in IEEE 754 double: 0x3FF0000000000000 */
        FREG f = ieee754_to_fr(0x3FF0000000000000ULL);
        if (f.exp == FP_EXP_BIAS && (f.mant & FP_INTEGER_BIT)) {
            LOG_I("  PASS: IEEE754 1.0 -> FREG");
            pass++;
        } else {
            LOG_E("  FAIL: IEEE754 1.0: exp=0x%X mant=0x%llX",
                  f.exp, (unsigned long long)f.mant);
            fail++;
        }
    }

    /* test NaTVal */
    {
        FREG nv = fr_make_natval();
        if (fr_is_natval(&nv)) {
            LOG_I("  PASS: NaTVal created and detected");
            pass++;
        } else {
            LOG_E("  FAIL: NaTVal not detected");
            fail++;
        }
    }

    /* #5 instruction decode */
    LOG_I("--- Test 5: Instruction decode ---");

    /* nop encoding (all zeros) SHOULD decode as INST_NOP */
    {
        DecodedInst di;
        memset(&di, 0, sizeof(di));
        InstId id = slot_decode(SLOT_I, 0, &di);
        if (id == INST_NOP) {
            LOG_I("  PASS: I-slot NOP decoded correctly");
            pass++;
        } else {
            LOG_E("  FAIL: I-slot NOP decoded as %d (%s)",
                  id, inst_name(id));
            fail++;
        }
    }

    /* test inst_name */
    {
        const char *name = inst_name(INST_ADD);
        if (name && strcmp(name, "???") != 0) {
            LOG_I("  PASS: inst_name(INST_ADD) = \"%s\"", name);
            pass++;
        } else {
            LOG_E("  FAIL: inst_name(INST_ADD) = \"%s\"", name ? name : "NULL");
            fail++;
        }
    }

    /* #6 predication */
    LOG_I("--- Test 6: Predication ---");

    /* when predicate is false, instruction should be NOP */
    cpu->pr[0] = true;
    cpu->pr[5] = false;
    cpu_write_gr(cpu, 5, (GREG){100, false});
    cpu_write_gr(cpu, 6, (GREG){200, false});
    cpu_write_gr(cpu, 4, (GREG){999, false});  /* original value */
    {
        /* simulate predicated-off ADD */
        DecodedInst d = {0};
        d.inst_id = INST_ADD;
        d.qp = 5;  /* use PR5 which is false */
        d.r1 = 4; d.r2 = 5; d.r3 = 6;

        /* predicate check: PR[5] is false, so do not't execute */
        if (!cpu_read_pr(cpu, d.qp)) {
            LOG_I("  PASS: Predicated-off instruction correctly skipped");
            pass++;
        } else {
            LOG_E("  FAIL: Predicate should be false");
            fail++;
        }
    }

    /* #7 alat */
    LOG_I("--- Test 7: ALAT ---");

    extern void alat_write_entry(IA64CPU *cpu, bool fpreg, uint8_t reg,
                                  uint64_t pa, uint32_t size);
    extern int alat_check(IA64CPU *cpu, bool fpreg, uint8_t reg,
                           uint64_t pa, uint32_t size);
    extern void alat_invalidate_all(IA64CPU *cpu);

    {
        alat_invalidate_all(cpu);
        alat_write_entry(cpu, false, 8, 0x100000, 8);
        int hit = alat_check(cpu, false, 8, 0x100000, 8);
        if (hit) {
            LOG_I("  PASS: ALAT write + check hit");
            pass++;
        } else {
            LOG_E("  FAIL: ALAT check should hit");
            fail++;
        }

        alat_invalidate_all(cpu);
        hit = alat_check(cpu, false, 8, 0x100000, 8);
        if (!hit) {
            LOG_I("  PASS: ALAT invalidate all");
            pass++;
        } else {
            LOG_E("  FAIL: ALAT should miss after invalidate");
            fail++;
        }
    }

    /* summary  */
    LOG_I("=== Exec Tests: %d passed, %d failed ===", pass, fail);
}

/* device integration tests */

static void run_device_tests(IA64Machine *machine) {
    IA64CPU *cpu = machine->cpus[0];
    int pass = 0, fail = 0;

    LOG_I("=== Device Integration Tests ===");

    /* reset CPU state */
    cpu_reset(cpu);
    cpu->ip = make_ip(PHYS_MAIN_RAM_BASE, 0);

    /* test1 SAPIC interrupt delivery */
    LOG_I("--- Test 1: SAPIC ---");

    /* raise IRQ vector 0x30 on Local SAPIC 0 */
    local_sapic_raise_irq(&machine->local_sapic[0], 0x30);
    if (machine->local_sapic[0].irr[0] & (1ULL << 0x30)) {
        LOG_I("  PASS: IRR bit set for vector 0x30");
        pass++;
    } else {
        LOG_E("  FAIL: IRR bit not set for vector 0x30");
        fail++;
    }

    /* clear TPR.mi (init sets mi=1 which MASKS all interrupts) */
    machine->local_sapic[0].tpr = 0;

    /* read IVR (CR65) - it SHOULD return highest pending vector */
    uint64_t ivr = local_sapic_cr_read(&machine->local_sapic[0], 65);
    if (ivr == 0x30) {
        LOG_I("  PASS: IVR returned 0x30");
        pass++;
    } else {
        LOG_E("  FAIL: IVR returned 0x%llX (expected 0x30)",
              (unsigned long long)ivr);
        fail++;
    }

    /* IRR should be cleared now. hopefully */
    if (!(machine->local_sapic[0].irr[0] & (1ULL << 0x30))) {
        LOG_I("  PASS: IRR cleared after IVR read");
        pass++;
    } else {
        LOG_E("  FAIL: IRR not cleared");
        fail++;
    }

    /* TPR masking: set TPR to mask vectors < 0x40 */
    machine->local_sapic[0].tpr = (4ULL << 4);  /* pri=4 (mask vec<0x40) */
    local_sapic_raise_irq(&machine->local_sapic[0], 0x20);  /* below TPR */
    uint8_t pending = local_sapic_check_pending(&machine->local_sapic[0]);
    if (pending == 0) {
        LOG_I("  PASS: TPR masks low-priority vector");
        pass++;
    } else {
        LOG_E("  FAIL: TPR should mask vector 0x20");
        fail++;
    }
    /* CLEAR the IRR bit for clean state */
    machine->local_sapic[0].irr[0] &= ~(1ULL << 0x20);
    machine->local_sapic[0].tpr = 0;

    /* EOI test */
    machine->local_sapic[0].current_irq = 0x30;
    local_sapic_cr_write(&machine->local_sapic[0], 67, 0);
    if (machine->local_sapic[0].current_irq == -1) {
        LOG_I("  PASS: EOI cleared current IRQ");
        pass++;
    } else {
        LOG_E("  FAIL: EOI did not clear current_irq");
        fail++;
    }

    /* #2 pci config */
    LOG_I("--- Test 2: PCI ---");

    /* read host bridge vendor/device ide */
    uint32_t vid = pci_config_read(&machine->pci_bus, 0, 0, 0,
                                    PCI_VENDOR_ID, 2);
    if (vid == 0x8086) {
        LOG_I("  PASS: Host Bridge vendor = Intel (0x8086)");
        pass++;
    } else {
        LOG_E("  FAIL: Host Bridge vendor = 0x%04X", vid);
        fail++;
    }

    /* read ISA bridge device ID */
    uint32_t did = pci_config_read(&machine->pci_bus, 0, 7, 0,
                                    PCI_DEVICE_ID, 2);
    if (did == 0x7110) {
        LOG_I("  PASS: ISA bridge device = PIIX4 (0x7110)");
        pass++;
    } else {
        LOG_E("  FAIL: ISA bridge device = 0x%04X", did);
        fail++;
    }

    /* read IDE controller device ID */
    uint32_t ide_did = pci_config_read(&machine->pci_bus, 0, 7, 1,
                                        PCI_DEVICE_ID, 2);
    if (ide_did == 0x7111) {
        LOG_I("  PASS: IDE device = PIIX4 IDE (0x7111)");
        pass++;
    } else {
        LOG_E("  FAIL: IDE device = 0x%04X", ide_did);
        fail++;
    }

    /* #3 you art? (UART) */
    LOG_I("--- Test 3: UART ---");

    /* write to THR and check LSR */
    uart_io_write(&machine->uart[0], 0x3F8, 'A', 1);  /* write 'A' */
    uint64_t lsr = uart_io_read(&machine->uart[0], 0x3F8 + 5, 1);  /* LSR */
    if (lsr & 0x20) {  /* THRE bit */
        LOG_I("  PASS: UART THRE set after transmit");
        pass++;
    } else {
        LOG_E("  FAIL: UART THRE not set (LSR=0x%llX)",
              (unsigned long long)lsr);
        fail++;
    }

    /* inject char and read */
    uart_rx_char(&machine->uart[0], 'Z');
    uint64_t rbr = uart_io_read(&machine->uart[0], 0x3F8, 1);  /* RBR */
    if (rbr == 'Z') {
        LOG_I("  PASS: UART received 'Z'");
        pass++;
    } else {
        LOG_E("  FAIL: UART received 0x%llX (expected 'Z')",
              (unsigned long long)rbr);
        fail++;
    }

    /* #4 rtc */
    LOG_I("--- Test 4: RTC ---");

    /* read seconds register */
    rtc_io_write(&machine->rtc, 0x70, 0x00, 1);  /* index = seconds */
    uint64_t secs = rtc_io_read(&machine->rtc, 0x71, 1);
    /* BCD decode */
    int sec_val = ((secs >> 4) & 0xF) * 10 + (secs & 0xF);
    if (sec_val >= 0 && sec_val <= 59) {
        LOG_I("  PASS: RTC seconds = %d (BCD 0x%02llX)",
              sec_val, (unsigned long long)secs);
        pass++;
    } else {
        LOG_E("  FAIL: RTC seconds = %d (0x%02llX)",
              sec_val, (unsigned long long)secs);
        fail++;
    }

    /* #5 PAL procedures :) */
    LOG_I("--- Test 5: PAL ---");

    /* PAL_VM_INFO param 0: VA/PA bits */
    pal_dispatch(cpu, PAL_VM_INFO, 0, 0, 0);
    {
        uint64_t r8 = cpu_read_gr(cpu, 8).val;
        uint64_t r9 = cpu_read_gr(cpu, 9).val;
        if (r8 == 0) {
            uint8_t va_bits = (uint8_t)((r9 >> 8) & 0xFF);
            uint8_t pa_bits = (uint8_t)(r9 & 0xFF);
            if (va_bits == 61 && pa_bits == 50) {
                LOG_I("  PASS: PAL_VM_INFO VA=%u PA=%u", va_bits, pa_bits);
                pass++;
            } else {
                LOG_E("  FAIL: PAL_VM_INFO VA=%u PA=%u",
                      va_bits, pa_bits);
                fail++;
            }
        } else {
            LOG_E("  FAIL: PAL_VM_INFO status=%lld",
                  (long long)(int64_t)r8);
            fail++;
        }
    }

    /* PAL_FREQ_RATIOS: CPU ratio */
    pal_dispatch(cpu, PAL_FREQ_RATIOS, 0, 0, 0);
    {
        uint64_t r8 = cpu_read_gr(cpu, 8).val;
        uint64_t r9 = cpu_read_gr(cpu, 9).val;
        if (r8 == 0 && r9 == ((8ULL << 32) | 1ULL)) {
            LOG_I("  PASS: PAL_FREQ_RATIOS CPU=8:1");
            pass++;
        } else {
            LOG_E("  FAIL: PAL_FREQ_RATIOS status=%lld r9=0x%llX",
                  (long long)(int64_t)r8, (unsigned long long)r9);
            fail++;
        }
    }

    /* PAL_BRAND_INFO */
    pal_dispatch(cpu, PAL_BRAND_INFO, 0, 0, 0);
    {
        uint64_t r8 = cpu_read_gr(cpu, 8).val;
        uint64_t r9 = cpu_read_gr(cpu, 9).val;
        char brand[9] = {0};
        memcpy(brand, &r9, 8);
        if (r8 == 0 && brand[0] == 'E') {
            LOG_I("  PASS: PAL_BRAND_INFO = \"%s...\"", brand);
            pass++;
        } else {
            LOG_E("  FAIL: PAL_BRAND_INFO status=%lld brand=0x%llX",
                  (long long)(int64_t)r8, (unsigned long long)r9);
            fail++;
        }
    }

    /* #6 SAL procedures */
    LOG_I("--- Test 6: SAL ---");

    /* SAL_FREQ_BASE type 0 platform clock */
    sal_dispatch(cpu, SAL_FREQ_BASE, 0, 0, 0, 0, 0);
    {
        uint64_t r8 = cpu_read_gr(cpu, 8).val;
        uint64_t r9 = cpu_read_gr(cpu, 9).val;
        if (r8 == 0 && r9 == 100000000ULL) {
            LOG_I("  PASS: SAL_FREQ_BASE = 100MHz");
            pass++;
        } else {
            LOG_E("  FAIL: SAL_FREQ_BASE status=%lld freq=%llu",
                  (long long)(int64_t)r8, (unsigned long long)r9);
            fail++;
        }
    }

    /* SAL_PCI_CONFIG_READ- read Host Bridge vendor ID */
    {
        /* PCI addr: bus=0, dev=0, func=0, offset=0 */
        uint64_t pci_addr = 0x00000000ULL;
        sal_dispatch(cpu, SAL_PCI_CONFIG_READ, pci_addr, 2, 0, 0, 0);
        uint64_t r8 = cpu_read_gr(cpu, 8).val;
        uint64_t r9 = cpu_read_gr(cpu, 9).val;
        if (r8 == 0 && (r9 & 0xFFFF) == 0x8086) {
            LOG_I("  PASS: SAL_PCI_CFG_READ vendor=0x%04llX",
                  (unsigned long long)(r9 & 0xFFFF));
            pass++;
        } else {
            LOG_E("  FAIL: SAL_PCI_CFG_READ status=%lld val=0x%llX",
                  (long long)(int64_t)r8, (unsigned long long)r9);
            fail++;
        }
    }

    /* #7 ACPI tables */
    LOG_I("--- Test 7: ACPI ---");

    /* verify RSDP at ACPI_RSDP_PHYS */
    {
        int fault = 0;
        uint64_t sig_lo = phys_mem_read(cpu, ACPI_RSDP_PHYS, 8, &fault);
        if (!fault && sig_lo == 0x2052545020445352ULL) {  /* "RSD PTR " the */
            LOG_I("  PASS: RSDP signature verified");
            pass++;
        } else {
            LOG_E("  FAIL: RSDP signature = 0x%016llX fault=%d",
                  (unsigned long long)sig_lo, fault);
            fail++;
        }
    }

    /* verify RSDP checksum */
    {
        uint8_t rsdp_data[36];
        phys_mem_block_read(ACPI_RSDP_PHYS, rsdp_data, 36);
        uint8_t sum = acpi_checksum(rsdp_data, 36);
        if (sum == 0) {
            LOG_I("  PASS: RSDP checksum valid");
            pass++;
        } else {
            LOG_E("  FAIL: RSDP checksum = 0x%02X", sum);
            fail++;
        }
    }

    /* verify XSDT exists (read pointer from RSDP offset 24) */
    {
        int fault = 0;
        uint64_t xsdt_addr = phys_mem_read(cpu, ACPI_RSDP_PHYS + 24, 8, &fault);
        if (!fault && xsdt_addr == ACPI_TABLES_PHYS) {
            /* read XSDT signature */
            uint32_t xsdt_sig = (uint32_t)phys_mem_read(cpu, xsdt_addr, 4, &fault);
            if (!fault && xsdt_sig == 0x54445358U) {  /* "XSDT" the */
                LOG_I("  PASS: XSDT at 0x%llX signature valid",
                      (unsigned long long)xsdt_addr);
                pass++;
            } else {
                LOG_E("  FAIL: XSDT signature = 0x%08X", xsdt_sig);
                fail++;
            }
        } else {
            LOG_E("  FAIL: XSDT addr = 0x%llX fault=%d",
                  (unsigned long long)xsdt_addr, fault);
            fail++;
        }
    }

    /* test summary. */
    LOG_I("=== Phase 3 Tests: %d passed, %d failed ===", pass, fail);
}

/* argument parsing and entry point */

static void print_usage(const char *prog) {
    printf("Epic! - an Itanium emulator, made with :3\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -m <megs>       RAM size in MB (default: 2048)\n");
    printf("  -smp <n>        Number of CPUs (default: 1)\n");
    printf("  -cdrom <iso>    Boot from ISO image\n");
    printf("  -fda <img>      Boot from floppy image\n");
    printf("  -hda <img>      Hard disk image\n");
    printf("  -bios <path>    Custom firmware path\n");
    printf("  -serial <mode>  Serial backend: stdio, file:<path>, none\n");
    printf("  -cmdline <str>  Override the Linux kernel command line\n");
    printf("  -kernel <path>  Direct-boot an IA-64 kernel (PE32+ vmlinuz)\n");
    printf("  -initrd <path|start:size>  Initrd file or address:size pair\n");
    printf("  -trace          Enable trace-level logging for debugging\n");
    printf("  -test           Run all validation tests - debugging!!!!!!!!!!! fun\n");
    printf("  -dump-state     Dump CPU state after test\n");
    printf("  -help           Show this help\n");
}

static int load_host_file(const char *path, uint8_t **data_out,
                           size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("Cannot open host file: %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        LOG_E("Host file is empty or unreadable: %s", path);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        LOG_E("Out of memory loading %s (%ld bytes)", path, size);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        LOG_E("Short read on %s", path);
        return -1;
    }
    fclose(f);
    *data_out = buf;
    *size_out = (size_t)size;
    LOG_I("Loaded host file: %s (%ld bytes)", path, size);
    return 0;
}

static int parse_initrd_spec(const char *spec,
                             uint64_t *start_out,
                             uint64_t *size_out) {
    if (!spec || !start_out || !size_out) {
        return -1;
    }

    const char *colon = strchr(spec, ':');
    if (!colon || colon == spec || colon[1] == '\0') {
        return -1;
    }

    char *end = NULL;
    unsigned long long start = strtoull(spec, &end, 0);
    if (end != colon) {
        return -1;
    }

    unsigned long long size = strtoull(colon + 1, &end, 0);
    if (*end != '\0') {
        return -1;
    }

    *start_out = (uint64_t)start;
    *size_out = (uint64_t)size;
    return 0;
}

int main(int argc, char *argv[]) {
    uint64_t ram_mb = 2048;
    int num_cpus = 1;
    bool run_test = false;
    bool dump_state = false;
    bool trace_logging = false;
    const char *cdrom = NULL;
    const char *fda = NULL;
    const char *hda = NULL;
    const char *firmware = NULL;
    const char *serial_mode_str = "stdio";
    const char *kernel_cmdline = NULL;
    const char *initrd_spec = NULL;
    const char *kernel_path = NULL;

    /* parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            ram_mb = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-smp") == 0 && i + 1 < argc) {
            num_cpus = atoi(argv[++i]);
            if (num_cpus < 1) num_cpus = 1;
            if (num_cpus > 4) num_cpus = 4;
        } else if (strcmp(argv[i], "-cdrom") == 0 && i + 1 < argc) {
            cdrom = argv[++i];
        } else if (strcmp(argv[i], "-fda") == 0 && i + 1 < argc) {
            fda = argv[++i];
        } else if (strcmp(argv[i], "-hda") == 0 && i + 1 < argc) {
            hda = argv[++i];
        } else if (strcmp(argv[i], "-bios") == 0 && i + 1 < argc) {
            firmware = argv[++i];
        } else if (strcmp(argv[i], "-serial") == 0 && i + 1 < argc) {
            serial_mode_str = argv[++i];
        } else if (strcmp(argv[i], "-kernel") == 0 && i + 1 < argc) {
            kernel_path = argv[++i];
        } else if (strcmp(argv[i], "-cmdline") == 0 && i + 1 < argc) {
            kernel_cmdline = argv[++i];
        } else if (strcmp(argv[i], "-initrd") == 0 && i + 1 < argc) {
            initrd_spec = argv[++i];
        } else if (strcmp(argv[i], "-trace") == 0) {
            trace_logging = true;
        } else if (strcmp(argv[i], "-test") == 0) {
            run_test = true;
        } else if (strcmp(argv[i], "-dump-state") == 0) {
            dump_state = true;
        } else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* parse serial mode */
    int serial_mode = 0;  /* stdio */
    const char *serial_file = NULL;
    if (strcmp(serial_mode_str, "stdio") == 0) {
        serial_mode = 0;
    } else if (strncmp(serial_mode_str, "file:", 5) == 0) {
        serial_mode = 1;
        serial_file = serial_mode_str + 5;
    } else if (strcmp(serial_mode_str, "none") == 0) {
        serial_mode = 2;
    }

    if (kernel_cmdline != NULL) {
        efi_apply_boot_param_override(kernel_cmdline, 0, 0);
        LOG_I("EFI: using user kernel cmdline override: %s", kernel_cmdline);
    }

    if (initrd_spec != NULL && kernel_path == NULL) {
        /* only parse start:size when not direct-booting (THE FILE PATH IS handED LATEWR) */
        uint64_t initrd_start = 0;
        uint64_t initrd_size = 0;
        if (parse_initrd_spec(initrd_spec, &initrd_start, &initrd_size) != 0) {
            fprintf(stderr, "Invalid -initrd value (expected start:size): %s\n",
                    initrd_spec);
            return 1;
        }
        efi_apply_boot_param_override(kernel_cmdline ? kernel_cmdline : NULL,
                                      initrd_start,
                                      initrd_size);
        LOG_I("EFI: using initrd override 0x%llX/%llu bytes",
              (unsigned long long)initrd_start,
              (unsigned long long)initrd_size);
    }

    epic_log_set_level(epic_log_level_for_cli(trace_logging));

    LOG_I("Epic! - an Itanium emulator, made with :3");
    LOG_I("======================================");

    /* create see machine */
    IA64Machine *machine = machine_create("generic", ram_mb * 1024 * 1024, num_cpus,
                                            serial_mode, serial_file);
    if (!machine) {
        LOG_F("Failed to create machine");
        return 1;
    }

    machine->cdrom_path = cdrom;
    machine->fda_path = fda;
    machine->hda_path = hda;
    machine->firmware_path = firmware;

    /* attach storage (IDE pluis boot device probing) */
    machine_attach_storage(machine);

    /* set CPU entry point to FW ROM */
    for (int i = 0; i < num_cpus; i++) {
        machine->cpus[i]->ip = make_ip(PHYS_FIRMWARE_ROM_BASE, 0);
        machine->cpus[i]->cr[CR_IVA] =
            PHYS_FIRMWARE_ROM_BASE | FIRMWARE_IVT_OFFSET;
    }

    /* dump initial memory map */
    mem_dump_map(machine->memory);

    /* run tests, direct kernel boot, OR idle */
    if (run_test) {
        run_basic_cpu_tests(machine);
        run_exec_tests(machine);
        run_device_tests(machine);
    } else if (kernel_path) {
        /* direct kernel boot path*/
        LOG_I("=== Direct kernel boot ===");

        /* load kernel file from host into guest RAM staging area */
        uint8_t *kernel_data = NULL;
        size_t kernel_size = 0;
        if (load_host_file(kernel_path, &kernel_data, &kernel_size) != 0) {
            LOG_F("Failed to load kernel: %s", kernel_path);
            machine_destroy(machine);
            return 1;
        }

        /* write kernel file to staging area in guest RAM */
        uint64_t staging_addr = PHYS_MAIN_RAM_BASE;
        phys_mem_block_write(staging_addr, kernel_data, kernel_size);
        free(kernel_data);
        LOG_I("Kernel staged at guest phys 0x%llX (%zu bytes)",
              (unsigned long long)staging_addr, kernel_size);

        /* pee e load: parse headers, allocate pages, load sections, relocate yada yada*/
        PeLoadResult load_result;
        memset(&load_result, 0, sizeof(load_result));
        int rc = pe_load_image(staging_addr, (uint64_t)kernel_size, &load_result);
        if (rc != 0) {
            LOG_F("PE loader failed to load kernel (rc=%d)", rc);
            machine_destroy(machine);
            return 1;
        }
        g_last_loaded_image = load_result;
        LOG_I("Kernel loaded: entry=0x%llX base=0x%llX size=%llu",
              (unsigned long long)load_result.entry_point,
              (unsigned long long)load_result.image_base,
              (unsigned long long)load_result.image_size);

        /* load initrd IF specified */
        uint64_t initrd_phys_start = 0;
        uint64_t initrd_phys_size = 0;

        if (initrd_spec) {
            /* check if it's a file path (aka no colon) or start:size pair */
            if (strchr(initrd_spec, ':') == NULL) {
                /* file path- load from host */
                uint8_t *initrd_data = NULL;
                size_t initrd_file_size = 0;
                if (load_host_file(initrd_spec, &initrd_data,
                                   &initrd_file_size) != 0) {
                    LOG_F("Failed to load initrd: %s", initrd_spec);
                    machine_destroy(machine);
                    return 1;
                }
                /* place initrd 32MB into main RAM */
                initrd_phys_start = PHYS_MAIN_RAM_BASE + 0x2000000;
                initrd_phys_size = (uint64_t)initrd_file_size;
                phys_mem_block_write(initrd_phys_start, initrd_data,
                                     initrd_file_size);
                free(initrd_data);
                LOG_I("Initrd loaded at 0x%llX (%llu bytes)",
                      (unsigned long long)initrd_phys_start,
                      (unsigned long long)initrd_phys_size);
            } else {
                /* existing start:size format */
                parse_initrd_spec(initrd_spec, &initrd_phys_start,
                                  &initrd_phys_size);
            }
            efi_apply_boot_param_override(
                kernel_cmdline ? kernel_cmdline : NULL,
                initrd_phys_start, initrd_phys_size);
        }

        /* build boot params in FW ROM */
        efi_build_boot_param(initrd_phys_start, initrd_phys_size);

        /* read the ia-64 function descriptor at the entry point:
         * fdesc[0] = code entry address, fdesc[1] = gp */
        uint64_t fdesc[2];
        phys_mem_block_read(load_result.entry_point, fdesc, sizeof(fdesc));
        IA64CPU *bsp = machine->cpus[0];
        bsp->ip = fdesc[0];

        /* set IA-64 Linux boot registers where applicahbel! */
        cpu_write_gr(bsp, 28, (GREG){IA64_BOOT_PARAM_PHYS, false});
        cpu_write_gr(bsp, 1,  (GREG){fdesc[1], false});
        cpu_write_gr(bsp, 32, (GREG){load_result.image_handle, false});
        cpu_write_gr(bsp, 33, (GREG){EFI_SYSTAB_PHYS, false});

        LOG_I("=== Starting kernel execution ===");
        LOG_I("  IP = 0x%llX, GR28 = 0x%llX (boot_param)",
              (unsigned long long)bsp->ip,
              (unsigned long long)IA64_BOOT_PARAM_PHYS);
        /* dump descriptor at RVA 0x45C40 which causes illegal template crash */
        {
            uint8_t raw[16];
            uint64_t fw_desc_addr = load_result.image_base + 0x45C40;
            phys_mem_block_read(fw_desc_addr, raw, 16);
            uint64_t *qw = (uint64_t *)raw;
            LOG_I("DESC @RVA 0x45C40: code=0x%016llX gp=0x%016llX",
                  (unsigned long long)qw[0], (unsigned long long)qw[1]);
            uint8_t raw2[16];
            phys_mem_block_read(qw[0], raw2, 16);
            LOG_I("CODE target @0x%016llX: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  (unsigned long long)qw[0],
                  raw2[0], raw2[1], raw2[2], raw2[3], raw2[4], raw2[5], raw2[6], raw2[7],
                  raw2[8], raw2[9], raw2[10], raw2[11], raw2[12], raw2[13], raw2[14], raw2[15]);
        }

        /* Run the CPU! :DDD */
        g_running_cpu = bsp;
        signal(SIGINT, sigint_handler);
        cpu_run(bsp);
        g_running_cpu = NULL;

        LOG_I("=== Kernel exited after %llu instructions ===",
              (unsigned long long)bsp->inst_count);
    } else if (cdrom && machine->num_boot_devs > 0) {
        /* iso/cdrom */
        LOG_I("=== Booting from CD-ROM ISO ===");

        /* find the CD-ROM boot device */
        BootDevice *boot_dev = NULL;
        for (int i = 0; i < machine->num_boot_devs; i++) {
            if (machine->boot_dev[i].type == BOOT_DEV_CDROM) {
                boot_dev = &machine->boot_dev[i];
                break;
            }
        }
        if (!boot_dev) {
            LOG_F("No CD-ROM boot device found");
            machine_destroy(machine);
            return 1;
        }

        /* read BOOTIA64.EFI from ISO into host buffer where applicable, could be named something different in some weird edge case but not accounted for here, YET. */
        #define EFI_LOADER_MAX_SIZE (32 * 1024 * 1024)
        uint8_t *loader_buf = (uint8_t *)calloc(1, EFI_LOADER_MAX_SIZE);
        if (!loader_buf) {
            LOG_F("Out of memory for loader buffer");
            machine_destroy(machine);
            return 1;
        }

        int64_t loader_size = boot_find_loader(boot_dev, loader_buf,
                                                EFI_LOADER_MAX_SIZE);
        if (loader_size <= 0) {
            LOG_F("Failed to load EFI boot application from ISO");
            free(loader_buf);
            machine_destroy(machine);
            return 1;
        }

        /* write EFI loader to guest RAM staging area */
        uint64_t staging_addr = PHYS_MAIN_RAM_BASE;
        /* check loader_buf byte at offset 0x1E1 before writing */
        uint8_t *lb = (uint8_t *)loader_buf;
        LOG_E("DIAG: loader_buf[0x1E1]=0x%02X loader_buf[0x1E0]=0x%02X loader_size=%lld",
              lb[0x1E1], lb[0x1E0], (long long)loader_size);
        phys_mem_block_write(staging_addr, loader_buf, (size_t)loader_size);
        /* read back and verify */
        uint8_t verify[4];
        phys_mem_block_read(staging_addr + 0x1E0, verify, 4);
        LOG_E("DIAG: guest readback at 0x%llX+0x1E0: %02X %02X %02X %02X",
              (unsigned long long)staging_addr, verify[0], verify[1], verify[2], verify[3]);
        free(loader_buf);
        LOG_E("EFI loader staged at guest phys 0x%llX (%lld bytes)",
              (unsigned long long)staging_addr,
              (long long)loader_size);

        /* pe load the EFI application */
        PeLoadResult load_result;
        memset(&load_result, 0, sizeof(load_result));
        int rc = pe_load_image(staging_addr, (uint64_t)loader_size,
                               &load_result);
        if (rc != 0) {
            LOG_F("PE loader failed on EFI boot app (rc=%d)", rc);
            machine_destroy(machine);
            return 1;
        }
        g_last_loaded_image = load_result;
        LOG_E("EFI loader loaded: entry=0x%llX base=0x%llX size=%llu",
              (unsigned long long)load_result.entry_point,
              (unsigned long long)load_result.image_base,
              (unsigned long long)load_result.image_size);

        /* set up EFI handoff, the loader will make EFI calls */
        IA64CPU *bsp = machine->cpus[0];

        uint64_t fdesc[2];
        phys_mem_block_read(load_result.entry_point, fdesc, sizeof(fdesc));
        uint64_t code_entry = fdesc[0];
        uint64_t gp_value   = fdesc[1];

        LOG_I("IA-64 function descriptor: code=0x%llX gp=0x%llX",
              (unsigned long long)code_entry,
              (unsigned long long)gp_value);

        /* dump first 3 bundles at entry */
        uint8_t entry_buf[48];
        phys_mem_block_read(code_entry, entry_buf, 48);
        LOG_I("ENTRY[0x%llX]: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
              (unsigned long long)code_entry,
              entry_buf[0], entry_buf[1], entry_buf[2], entry_buf[3],
              entry_buf[4], entry_buf[5], entry_buf[6], entry_buf[7],
              entry_buf[8], entry_buf[9], entry_buf[10], entry_buf[11],
              entry_buf[12], entry_buf[13], entry_buf[14], entry_buf[15]);
        LOG_I("ENTRY[+16]: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
              entry_buf[16], entry_buf[17], entry_buf[18], entry_buf[19],
              entry_buf[20], entry_buf[21], entry_buf[22], entry_buf[23],
              entry_buf[24], entry_buf[25], entry_buf[26], entry_buf[27],
              entry_buf[28], entry_buf[29], entry_buf[30], entry_buf[31]);
        LOG_I("ENTRY[+32]: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
              entry_buf[32], entry_buf[33], entry_buf[34], entry_buf[35],
              entry_buf[36], entry_buf[37], entry_buf[38], entry_buf[39],
              entry_buf[40], entry_buf[41], entry_buf[42], entry_buf[43],
              entry_buf[44], entry_buf[45], entry_buf[46], entry_buf[47]);

        bsp->ip = code_entry;
        cpu_write_gr(bsp, 1,  (GREG){gp_value, false});
        cpu_write_gr(bsp, 32, (GREG){load_result.image_handle, false});
        cpu_write_gr(bsp, 33, (GREG){EFI_SYSTAB_PHYS, false});

        bsp->ar[AR_LC] = 0xFFFF; 
        bsp->ar[AR_EC] = 0;       

        LOG_I("=== Starting EFI boot application ===");
        LOG_I("  IP = 0x%llX, GR32 = handle, GR33 = 0x%llX (systab)",
              (unsigned long long)bsp->ip,
              (unsigned long long)EFI_SYSTAB_PHYS);
        /* dump descriptor at RVA 0x45C40 which causes illegal template crash */
        {
            uint8_t raw[16];
            uint64_t fw_desc_addr = load_result.image_base + 0x45C40;
            phys_mem_block_read(fw_desc_addr, raw, 16);
            uint64_t *qw = (uint64_t *)raw;
            LOG_I("DESC @RVA 0x45C40: code=0x%016llX gp=0x%016llX",
                  (unsigned long long)qw[0], (unsigned long long)qw[1]);
        }
        {
            uint8_t raw[16];
            uint64_t fw_desc_addr = load_result.image_base + 0x45C90;
            phys_mem_block_read(fw_desc_addr, raw, 16);
            uint64_t *qw = (uint64_t *)raw;
            LOG_I("DESC @RVA 0x45C90: code=0x%016llX gp=0x%016llX",
                  (unsigned long long)qw[0], (unsigned long long)qw[1]);
        }
        {
            uint8_t raw[16];
            uint64_t fw_desc_addr = load_result.image_base + 0x45CB0;
            phys_mem_block_read(fw_desc_addr, raw, 16);
            uint64_t *qw = (uint64_t *)raw;
            LOG_I("DESC @RVA 0x45CB0: code=0x%016llX gp=0x%016llX",
                  (unsigned long long)qw[0], (unsigned long long)qw[1]);
        }

        /* Run the CPU! :) */
        g_running_cpu = bsp;
        signal(SIGINT, sigint_handler);
        cpu_run(bsp);
        g_running_cpu = NULL;

        LOG_I("=== Boot application exited after %llu instructions ===",
              (unsigned long long)bsp->inst_count);
    } else {
        LOG_I("No -test, -kernel, or -cdrom specified. Use -help for options.");
        LOG_I("CPU %d starting at IP=0x%016llX",
              0, (unsigned long long)machine->cpus[0]->ip);
    }

    if (dump_state) {
        cpu_dump_state(machine->cpus[0]);
        cpu_dump_tlb(machine->cpus[0]);
        cpu_dump_rse(machine->cpus[0]);
    }

    /* just some cleanup :3 */
    machine_destroy(machine);

    return 0;
}
