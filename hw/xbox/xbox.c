/*
 * QEMU Xbox System Emulator
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#ifdef _WIN32
#include "accel/tcg/xbox-fastmem.h"
#include "exec/cputlb.h"
#include "system/tcg.h"
#include "migration/vmstate.h"
#include "cpu.h"
#endif
#include "qemu/option.h"
#include "qemu/datadir.h"
#include "hw/hw.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
#include "hw/i386/kvm/clock.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/ide/pci.h"
#include "system/system.h"
#include "system/kvm.h"
#include "kvm/kvm_i386.h"
#include "hw/dma/i8257.h"

#include "hw/sysbus.h"
#include "system/arch_init.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "cpu.h"

#include "qapi/error.h"
#include "qemu/error-report.h"

#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "hw/rtc/mc146818rtc.h"

#include "hw/xbox/xbox_pci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "hw/xbox/mcpx/apu/apu.h"

#include "hw/xbox/xbox.h"
#include "smbus.h"

#define MAX_IDE_BUS 2

#if defined(CONFIG_UWP) && defined(XEMU_UWP_BOOT_TRACE)
#define XBOX_UWP_TRACE(step) \
    fprintf(stderr, "xemu UWP: xbox_init_common " step "\n")
#define XBOX_UWP_FLASH_TRACE(step) \
    fprintf(stderr, "xemu UWP: xbox_flash_init " step "\n")
#define XBOX_UWP_MEMORY_TRACE(step) \
    fprintf(stderr, "xemu UWP: xbox_memory_init " step "\n")
#else
#define XBOX_UWP_TRACE(step) do { } while (0)
#define XBOX_UWP_FLASH_TRACE(step) do { } while (0)
#define XBOX_UWP_MEMORY_TRACE(step) do { } while (0)
#endif

/* FIXME: Clean this up and propagate errors to UI */
static void xbox_flash_init(MachineState *ms, MemoryRegion *rom_memory)
{
    const uint32_t rom_start = 0xFF000000;
    const char *bios_name;

    XBOX_UWP_FLASH_TRACE("locating BIOS image");

    /* Locate BIOS ROM image */
    bios_name = ms->firmware ?: "bios.bin";

    int failed_to_load_bios = 1;
    char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    uint32_t bios_size = 256 * 1024;

    if (filename != NULL) {
        int bios_file_size = get_image_size(filename, NULL);
        if ((bios_file_size > 0) && ((bios_file_size % 65536) == 0)) {
            failed_to_load_bios = 0;
            bios_size = bios_file_size;
        }
    }

    XBOX_UWP_FLASH_TRACE("allocating BIOS buffer");
    char *bios_data = g_malloc(bios_size);
    assert(bios_data != NULL);

    if (!failed_to_load_bios && (filename != NULL)) {
        XBOX_UWP_FLASH_TRACE("reading BIOS image");
        /* Read BIOS ROM into memory */
        failed_to_load_bios = 1;
        int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
        if (fd >= 0) {
            int rc = read(fd, bios_data, bios_size);
            if (rc == bios_size) {
                failed_to_load_bios = 0;
            }
            close(fd);
        }
    }

    if (failed_to_load_bios) {
        fprintf(stderr, "Failed to load BIOS '%s'\n", filename ? filename : "(null)");
        memset(bios_data, 0xff, bios_size);
    }
    if (filename != NULL) {
        g_free(filename);
    }

    /* Create BIOS region */
    XBOX_UWP_FLASH_TRACE("creating BIOS ROM region");
    MemoryRegion *bios;
    bios = g_malloc(sizeof(*bios));
    assert(bios != NULL);
    memory_region_init_rom(bios, NULL, "xbox.bios", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.bios", bios_data, bios_size, rom_start);

    /* Mirror ROM from 0xff000000 - 0xffffffff */
    XBOX_UWP_FLASH_TRACE("mapping BIOS ROM mirrors");
    uint32_t map_loc;
    for (map_loc = rom_start; map_loc >= rom_start; map_loc += bios_size) {
        MemoryRegion *map_bios = g_malloc(sizeof(*map_bios));
        memory_region_init_alias(map_bios, NULL, "pci-bios", bios, 0,
                                 bios_size);
        memory_region_add_subregion(rom_memory, map_loc, map_bios);
        memory_region_set_readonly(map_bios, true);
    }

    /* Create MCPX Boot ROM memory region
     *
     * For performance reasons, the overlay region should be page-aligned.
     * To do this, we simply make the memory region size equal to the size
     * of the BIOS image, and then overlay the boot ROM contents on top.
     *
     * Additionally, retail 1.1+ kernels have a quirk in very early boot stage
     * that depends on physical CPU WB caching behavior to briefly store a
     * computed value to a location in ROM and read it back in the next
     * instruction. Because we cannot emulate this cache behavior accurately,
     * work around this quirk by making this MCPX ROM region writable. When the
     * ROM is disabled during boot, any apparent writes to the region will be
     * discarded.
     *
     * Offending code which writes to ROM:
     *   sub ds:0FFFFD52Ch, eax
     *   mov eax, ds:0FFFFD52Ch
     */
    const char *bootrom_file =
        object_property_get_str(qdev_get_machine(), "bootrom", NULL);

    if ((bootrom_file != NULL) && *bootrom_file) {
        XBOX_UWP_FLASH_TRACE("locating MCPX boot ROM");
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bootrom_file);
        assert(filename);

        int bootrom_size = get_image_size(filename, NULL);
        if (bootrom_size != 512) {
            fprintf(stderr, "MCPX bootrom should be 512 bytes, got %d\n",
                    bootrom_size);
            exit(1);
            return;
        }

        /* Read in MCPX ROM over last 512 bytes of BIOS data */
        XBOX_UWP_FLASH_TRACE("reading MCPX boot ROM");
        int fd = qemu_open(filename, O_RDONLY | O_BINARY, NULL);
        assert(fd >= 0);
        int rc = read(fd, bios_data + bios_size - bootrom_size, bootrom_size);
        assert(rc == bootrom_size);
        close(fd);
        g_free(filename);
    }

    // Leave last BIOS image overlay writeable to satisfy cache dependency
    XBOX_UWP_FLASH_TRACE("creating MCPX overlay RAM");
    MemoryRegion *mcpx = g_malloc(sizeof(MemoryRegion));
    memory_region_init_ram(mcpx, NULL, "xbox.mcpx", bios_size, &error_fatal);
    rom_add_blob_fixed("xbox.mcpx", bios_data, bios_size, -bios_size);
    memory_region_add_subregion_overlap(rom_memory, -bios_size, mcpx, 1);

    g_free(bios_data); /* duplicated by `rom_add_blob_fixed` */
    XBOX_UWP_FLASH_TRACE("finished");
}

static void xbox_memory_init(PCMachineState *pcms,
                             MemoryRegion *system_memory,
                             MemoryRegion *rom_memory,
                             MemoryRegion **ram_memory)
{
    // int linux_boot, i;
    MemoryRegion *ram;//, *option_rom_mr;
    // FWCfgState *fw_cfg;
    MachineState *machine = MACHINE(pcms);
    // PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);

    // linux_boot = (machine->kernel_filename != NULL);

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    XBOX_UWP_MEMORY_TRACE("allocating guest RAM");
    ram = g_malloc(sizeof(*ram));
#ifdef _WIN32
    {
        /* Section-backed RAM lets the vCPU mirror guest pages directly. */
        void *direct = xbox_fastmem_alloc_ram(machine->ram_size);

        if (direct != NULL) {
            memory_region_init_ram_ptr(ram, NULL, "xbox.ram",
                                       machine->ram_size, direct);
            vmstate_register_ram_global(ram);
            xbox_fastmem_set_ram(direct, machine->ram_size,
                                 MMU_KNOSMAP32_IDX);
        } else {
            memory_region_init_ram(ram, NULL, "xbox.ram",
                                   machine->ram_size, &error_fatal);
        }
    }
#else
    memory_region_init_ram(ram, NULL, "xbox.ram",
                           machine->ram_size, &error_fatal);
#endif

    *ram_memory = ram;
    memory_region_add_subregion(system_memory, 0, ram);
    if (tcg_enabled()) {
        tlb_set_dirty_filter_ram(memory_region_get_ram_ptr(ram),
                                 machine->ram_size);
    }

    XBOX_UWP_MEMORY_TRACE("initializing flash and boot ROM");
    xbox_flash_init(machine, rom_memory);
    XBOX_UWP_MEMORY_TRACE("cleaning unused PC system flash");
    pc_system_flash_cleanup_unused(pcms);
    XBOX_UWP_MEMORY_TRACE("finished");
}

/* PC hardware initialisation */
static void xbox_init(MachineState *machine)
{
    xbox_init_common(machine, NULL, NULL);
}

void xbox_init_common(MachineState *machine,
                      PCIBus **pci_bus_out,
                      ISABus **isa_bus_out)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    // MemoryRegion *system_io = get_system_io();

    PCIBus *pci_bus;
    ISABus *isa_bus;

    // qemu_irq *i8259;
    // qemu_irq smi_irq; // XBOX_TODO: SMM support?

    GSIState *gsi_state;

    // DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    // BusState *idebus[MAX_IDE_BUS];
    MC146818RtcState *rtc_state;
    ISADevice *pit = NULL;
    int pit_isa_irq = 0;
    qemu_irq pit_alt_irq = NULL;
    // ISADevice *pit;

    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;

    I2CBus *smbus;
    PCIBus *agp_bus;

    XBOX_UWP_TRACE("entering x86 CPU init");
    x86_cpus_init(x86ms, pcmc->default_cpu_version);
    XBOX_UWP_TRACE("finished x86 CPU init");

    if (kvm_enabled()) {
        XBOX_UWP_TRACE("creating KVM clock");
        kvmclock_create(pcmc->kvmclock_create_always);
        XBOX_UWP_TRACE("finished KVM clock");
    }

    XBOX_UWP_TRACE("creating PCI memory region");
    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    // pc_guest_info_init(pcms);

    /* allocate ram and load rom/bios */
    XBOX_UWP_TRACE("entering Xbox RAM/flash init");
    xbox_memory_init(pcms, system_memory, rom_memory, &ram_memory);
    XBOX_UWP_TRACE("finished Xbox RAM/flash init");

    XBOX_UWP_TRACE("creating GSI/interrupt controller state");
    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);
    XBOX_UWP_TRACE("finished GSI/interrupt controller state");

    XBOX_UWP_TRACE("creating Xbox PCI/ISA/SMBus/AGP host");
    xbox_pci_init(x86ms->gsi,
                  get_system_memory(), get_system_io(),
                  pci_memory, ram_memory, rom_memory,
                  &pci_bus,
                  &isa_bus,
                  &smbus,
                  &agp_bus);
    XBOX_UWP_TRACE("finished Xbox PCI/ISA/SMBus/AGP host");

    pcms->pcibus = pci_bus;

    XBOX_UWP_TRACE("registering ISA input IRQs");
    isa_bus_register_input_irqs(isa_bus, x86ms->gsi);
    XBOX_UWP_TRACE("finished ISA input IRQ registration");

    XBOX_UWP_TRACE("creating PIC");
    pc_i8259_create(isa_bus, gsi_state->i8259_irq);
    XBOX_UWP_TRACE("finished PIC");

    if (tcg_enabled()) {
        XBOX_UWP_TRACE("registering FERR IRQ");
        x86_register_ferr_irq(x86ms->gsi[13]);
        XBOX_UWP_TRACE("finished FERR IRQ registration");
    }

    /* init basic PC hardware */
    XBOX_UWP_TRACE("creating RTC");
    rtc_state = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc_state);
    XBOX_UWP_TRACE("finished RTC");

    if (kvm_pit_in_kernel()) {
        XBOX_UWP_TRACE("creating KVM PIT");
        pit = kvm_pit_init(isa_bus, 0x40);
        XBOX_UWP_TRACE("finished KVM PIT");
    } else {
        XBOX_UWP_TRACE("creating i8254 PIT");
        pit = i8254_pit_init(isa_bus, 0x40, pit_isa_irq, pit_alt_irq);
        XBOX_UWP_TRACE("finished i8254 PIT");
    }

    XBOX_UWP_TRACE("creating i8257 DMA controller");
    i8257_dma_init(OBJECT(machine), isa_bus, 0);
    XBOX_UWP_TRACE("finished i8257 DMA controller");

    XBOX_UWP_TRACE("realizing PC speaker");
    object_property_set_link(OBJECT(pcms->pcspk), "pit",
                             OBJECT(pit), &error_fatal);
    isa_realize_and_unref(pcms->pcspk, isa_bus, &error_fatal);
    XBOX_UWP_TRACE("finished PC speaker");

    XBOX_UWP_TRACE("creating PIIX3 IDE controller");
    PCIDevice *dev = pci_create_simple(pci_bus, PCI_DEVFN(9, 0), "piix3-ide");
    pci_ide_create_devs(dev);
    XBOX_UWP_TRACE("finished PIIX3 IDE controller and drives");
    // idebus[0] = qdev_get_child_bus(&dev->qdev, "ide.0");
    // idebus[1] = qdev_get_child_bus(&dev->qdev, "ide.1");

    /* smbus devices */
    XBOX_UWP_TRACE("creating Xbox SMC SMBus device");
    smbus_xbox_smc_init(smbus, 0x10);
    XBOX_UWP_TRACE("finished Xbox SMC SMBus device");

    const char *video_encoder =
        object_property_get_str(qdev_get_machine(), "video-encoder", NULL);

    if (strcmp(video_encoder, "xcalibur") != 0) {
        if (!strcmp(video_encoder, "conexant")) {
            XBOX_UWP_TRACE("creating Conexant video encoder SMBus device");
            smbus_cx25871_init(smbus, 0x45);
            XBOX_UWP_TRACE("finished Conexant video encoder SMBus device");
        } else if (!strcmp(video_encoder, "focus")) {
            XBOX_UWP_TRACE("creating Focus video encoder SMBus device");
            smbus_fs454_init(smbus, 0x6A);
            XBOX_UWP_TRACE("finished Focus video encoder SMBus device");
        }
        XBOX_UWP_TRACE("creating ADM1032 thermal monitor SMBus device");
        smbus_adm1032_init(smbus, 0x4C);
        XBOX_UWP_TRACE("finished ADM1032 thermal monitor SMBus device");
    } else {
        XBOX_UWP_TRACE("creating Xcalibur video encoder SMBus device");
        smbus_xcalibur_init(smbus, 0x70);
        XBOX_UWP_TRACE("finished Xcalibur video encoder SMBus device");
    }

    /* USB */
    XBOX_UWP_TRACE("realizing OHCI USB controller at PCI 3:0");
    PCIDevice *usb1 = pci_new(PCI_DEVFN(3, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb1->qdev, "num-ports", 4);
    pci_realize_and_unref(usb1, pci_bus, &error_fatal);
    XBOX_UWP_TRACE("finished OHCI USB controller at PCI 3:0");

    XBOX_UWP_TRACE("realizing OHCI USB controller at PCI 2:0");
    PCIDevice *usb0 = pci_new(PCI_DEVFN(2, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb0->qdev, "num-ports", 4);
    pci_realize_and_unref(usb0, pci_bus, &error_fatal);
    XBOX_UWP_TRACE("finished OHCI USB controller at PCI 2:0");

    /* Ethernet! */
    XBOX_UWP_TRACE("realizing NVNet ethernet device");
    PCIDevice *nvnet = pci_new(PCI_DEVFN(4, 0), "nvnet");
    qemu_configure_nic_device(DEVICE(nvnet), true, "nvnet");
    pci_realize_and_unref(nvnet, pci_bus, &error_fatal);
    XBOX_UWP_TRACE("finished NVNet ethernet device");

    /* APU! */
    XBOX_UWP_TRACE("realizing MCPX APU audio device");
    mcpx_apu_init(pci_bus, PCI_DEVFN(5, 0), ram_memory);
    XBOX_UWP_TRACE("finished MCPX APU audio device");

    /* ACI! */
    XBOX_UWP_TRACE("realizing MCPX ACI audio codec interface");
    pci_create_simple(pci_bus, PCI_DEVFN(6, 0), "mcpx-aci");
    XBOX_UWP_TRACE("finished MCPX ACI audio codec interface");

    /* GPU! */
    XBOX_UWP_TRACE("realizing NV2A GPU device");
    nv2a_init(agp_bus, PCI_DEVFN(0, 0), ram_memory);
    XBOX_UWP_TRACE("finished NV2A GPU device");

    /* FIXME: Stub the memory controller */
    XBOX_UWP_TRACE("creating PCI memory controller stub");
    pci_create_simple(pci_bus, PCI_DEVFN(0, 3), "pci-testdev");
    XBOX_UWP_TRACE("finished PCI memory controller stub");

    if (pci_bus_out) {
        *pci_bus_out = pci_bus;
    }
    if (isa_bus_out) {
        *isa_bus_out = isa_bus;
    }
    XBOX_UWP_TRACE("finished");
}

static char *machine_get_bootrom(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->bootrom);
}

static void machine_set_bootrom(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    g_free(ms->bootrom);
    ms->bootrom = g_strdup(value);
}

static char *machine_get_avpack(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->avpack);
}

static void machine_set_avpack(Object *obj, const char *value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (!xbox_smc_avpack_to_reg(value, NULL)) {
        error_setg(errp, "-machine avpack=%s: unsupported option", value);
        xbox_smc_append_avpack_hint(errp);
        return;
    }

    g_free(ms->avpack);
    ms->avpack = g_strdup(value);
}

static void machine_set_short_animation(Object *obj, bool value, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    ms->short_animation = value;
}

static bool machine_get_short_animation(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);
    return ms->short_animation;
}

static char *machine_get_smc_version(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->smc_version);
}

static void machine_set_smc_version(Object *obj, const char *value,
                                    Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strlen(value) != 3) {
        error_setg(errp, "-machine smc-version=%s: unsupported option", value);
        xbox_smc_append_smc_version_hint(errp);
        return;
    }

    g_free(ms->smc_version);
    ms->smc_version = g_strdup(value);
}

static char *machine_get_video_encoder(Object *obj, Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    return g_strdup(ms->video_encoder);
}

static void machine_set_video_encoder(Object *obj, const char *value,
                                      Error **errp)
{
    XboxMachineState *ms = XBOX_MACHINE(obj);

    if (strcmp(value, "conexant") != 0 && strcmp(value, "focus") != 0 &&
        strcmp(value, "xcalibur") != 0) {
        error_setg(errp, "-machine video_encoder=%s: unsupported option",
                   value);
        error_append_hint(
            errp, "Valid options are: conexant (default), focus, xcalibur\n");
        return;
    }

    g_free(ms->video_encoder);
    ms->video_encoder = g_strdup(value);
}

static void xbox_machine_options(MachineClass *m)
{
    ObjectClass *oc = OBJECT_CLASS(m);

    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
    m->desc              = "Microsoft Xbox";
    m->max_cpus          = 1;
    m->option_rom_has_mr = true;
    m->rom_file_has_mr   = false;
    m->no_floppy         = 1,
    m->no_cdrom          = 1,
    m->default_cpu_type  = X86_CPU_TYPE_NAME("pentium3");
    m->is_default        = true;
    m->default_nic       = "nvnet";

    pcmc->pci_enabled         = true;
    pcmc->has_acpi_build      = false;
    pcmc->smbios_defaults     = false;
    pcmc->gigabyte_align      = false;
    pcmc->smbios_legacy_mode  = true;
    pcmc->has_reserved_memory = false;

    object_class_property_add_str(oc, "bootrom", machine_get_bootrom,
                                  machine_set_bootrom);
    object_class_property_set_description(oc, "bootrom", "Xbox bootrom file");

    object_class_property_add_str(oc, "avpack", machine_get_avpack,
                                  machine_set_avpack);
    object_class_property_set_description(
        oc, "avpack",
        "Xbox video connector: composite, scart, svideo, vga, rfu, hdtv "
        "(default), none");

    object_class_property_add_bool(oc, "short-animation",
                                   machine_get_short_animation,
                                   machine_set_short_animation);
    object_class_property_set_description(oc, "short-animation",
                                          "Skip Xbox boot animation");

    object_class_property_add_str(oc, "smc-version", machine_get_smc_version,
                                  machine_set_smc_version);
    object_class_property_set_description(
        oc, "smc-version", "Set the SMC version number, default is P01");

    object_class_property_add_str(oc, "video-encoder",
                                  machine_get_video_encoder,
                                  machine_set_video_encoder);
    object_class_property_set_description(
        oc, "video-encoder",
        "Set the encoder presented to the OS: conexant (default), focus, "
        "xcalibur");
}

static inline void xbox_machine_initfn(Object *obj)
{
    object_property_set_str(obj, "avpack", "hdtv", &error_fatal);
    object_property_set_bool(obj, "short-animation", false, &error_fatal);
    object_property_set_str(obj, "smc-version", "P01", &error_fatal);
    object_property_set_str(obj, "video-encoder", "conexant", &error_fatal);
}

static void xbox_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    xbox_machine_options(mc);
    mc->init = xbox_init;
}

static const TypeInfo pc_machine_type_xbox = {
    .name = TYPE_XBOX_MACHINE,
    .parent = TYPE_PC_MACHINE,
    .abstract = false,
    .instance_size = sizeof(XboxMachineState),
    .instance_init = xbox_machine_initfn,
    .class_size = sizeof(XboxMachineClass),
    .class_init = xbox_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         // { TYPE_HOTPLUG_HANDLER },
         // { TYPE_NMI },
         { }
    },
};

static void pc_machine_init_xbox(void)
{
    type_register_static(&pc_machine_type_xbox);
}

type_init(pc_machine_init_xbox)
