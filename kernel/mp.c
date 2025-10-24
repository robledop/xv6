// Multiprocessor support
// Search memory for MP description structures.
// http://developer.intel.com/design/pentium/datashts/24201606.pdf

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mp.h"
#include "x86.h"
#include "proc.h"

struct cpu cpus[NCPU];
int ncpu;
u8 ioapicid;

static int mpinit_legacy(void);
static int acpi_init(void);

struct acpi_rsdp
{
    char signature[8];
    u8 checksum;
    char oemid[6];
    u8 revision;
    u32 rsdt_addr;
} __attribute__((packed));

struct acpi_rsdp_v2
{
    struct acpi_rsdp v1;
    u32 length;
    unsigned long long xsdt_addr;
    u8 extended_checksum;
    u8 reserved[3];
} __attribute__((packed));

struct acpi_sdt_header
{
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oemid[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __attribute__((packed));

struct acpi_madt
{
    struct acpi_sdt_header header;
    u32 lapic_addr;
    u32 flags;
} __attribute__((packed));

struct acpi_madt_entry
{
    u8 type;
    u8 length;
} __attribute__((packed));

struct acpi_madt_lapic
{
    struct acpi_madt_entry header;
    u8 acpi_processor_id;
    u8 apic_id;
    u32 flags;
} __attribute__((packed));

struct acpi_madt_ioapic
{
    struct acpi_madt_entry header;
    u8 ioapic_id;
    u8 reserved;
    u32 ioapic_addr;
    u32 gsi_base;
} __attribute__((packed));

struct acpi_madt_lapic_override
{
    struct acpi_madt_entry header;
    u16 reserved;
    unsigned long long lapic_addr;
} __attribute__((packed));

struct acpi_madt_x2apic
{
    struct acpi_madt_entry header;
    u16 reserved;
    u32 x2apic_id;
    u32 flags;
    u32 acpi_processor_uid;
} __attribute__((packed));

static u8
sum(u8* addr, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++)
        sum += addr[i];
    return sum;
}

// Look for an MP structure in the len bytes at addr.
static struct mp*
mpsearch1(u32 a, int len)
{
    u8* addr = P2V(a);
    u8* e = addr + len;
    for (u8* p = addr; p < e; p += sizeof(struct mp))
        if (memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct mp)) == 0)
            return (struct mp*)p;
    return nullptr;
}

// Search for the MP Floating Pointer Structure, which according to the
// spec is in one of the following three locations:
// 1) in the first KB of the EBDA;
// 2) in the last KB of system base memory;
// 3) in the BIOS ROM between 0xE0000 and 0xFFFFF.
static struct mp* mpsearch(void)
{
    u32 p;
    struct mp* mp;

    u8* bda = (u8*)P2V(0x400);
    if ((p = ((bda[0x0F] << 8) | bda[0x0E]) << 4))
    {
        if ((mp = mpsearch1(p, 1024)))
            return mp;
    }
    else
    {
        p = ((bda[0x14] << 8) | bda[0x13]) * 1024;
        if ((mp = mpsearch1(p - 1024, 1024)))
            return mp;
    }
    return mpsearch1(0xF0000, 0x10000);
}

// Search for an MP configuration table.  For now,
// don't accept the default configurations (physaddr == 0).
// Check for the correct signature, calculate the checksum and,
// if correct, check the version.
// To do: check extended table checksum.
static void record_cpu_apicid(u32 apicid)
{
    for (int i = 0; i < ncpu; i++)
        if (cpus[i].apicid == apicid)
            return;

    if (ncpu < NCPU)
        cpus[ncpu++].apicid = apicid;
}

static struct mpconf* mpconfig(struct mp** pmp)
{
    struct mp* mp;

    if ((mp = mpsearch()) == nullptr || mp->physaddr == nullptr)
        return nullptr;
    struct mpconf* conf = (struct mpconf*)P2V((u32)mp->physaddr);
    if (memcmp(conf, "PCMP", 4) != 0)
        return nullptr;
    if (conf->version != 1 && conf->version != 4)
        return nullptr;
    if (sum((u8*)conf, conf->length) != 0)
        return nullptr;
    *pmp = mp;
    return conf;
}

static int mpinit_legacy(void)
{
    u8 *p, *e;
    struct mp* mp;
    struct mpconf* conf;

    if ((conf = mpconfig(&mp)) == nullptr)
        return 0;

    int ismp_ = 1;
    lapic = (u32*)conf->lapicaddr;
    for (p = (u8*)(conf + 1), e = (u8*)conf + conf->length; p < e;)
    {
        switch (*p)
        {
        case MPPROC:
            {
                struct mpproc* proc = (struct mpproc*)p;
                record_cpu_apicid(proc->apicid); // apicid may differ from ncpu
                p += sizeof(struct mpproc);
                continue;
            }
        case MPIOAPIC:
            {
                struct mpioapic* ioapic = (struct mpioapic*)p;
                ioapicid = ioapic->apicno;
                p += sizeof(struct mpioapic);
                continue;
            }
        case MPBUS:
        case MPIOINTR:
        case MPLINTR:
            p += 8;
            continue;
        default:
            ismp_ = 0;
            break;
        }
    }

    if (!ismp_)
        return 0;

    if (mp->imcrp)
    {
        // Bochs doesn't support IMCR, so this doesn't run on Bochs.
        // But it would on real hardware.
        outb(0x22, 0x70); // Select IMCR
        outb(0x23, inb(0x23) | 1); // Mask external interrupts.
    }

    return ncpu > 0;
}

static struct acpi_rsdp* acpi_rsdp_search(u32 phys_addr, int len)
{
    u8* addr = P2V(phys_addr);
    u8* end = addr + len;
    for (u8* p = addr; p < end; p += 16)
        if (memcmp(p, "RSD PTR ", 8) == 0)
        {
            struct acpi_rsdp* rsdp = (struct acpi_rsdp*)p;
            u32 length = sizeof(struct acpi_rsdp);
            if (rsdp->revision >= 2)
            {
                struct acpi_rsdp_v2* rsdp2 = (struct acpi_rsdp_v2*)p;
                if (rsdp2->length >= sizeof(struct acpi_rsdp))
                    length = rsdp2->length;
            }
            if (sum(p, length) == 0)
                return rsdp;
        }
    return nullptr;
}

static struct acpi_rsdp* acpi_find_rsdp(void)
{
    u8* bda = (u8*)P2V(0x400);
    u32 ebda_segment = (bda[0x0F] << 8) | bda[0x0E];
    if (ebda_segment)
    {
        struct acpi_rsdp* rsdp = acpi_rsdp_search(ebda_segment << 4, 1024);
        if (rsdp)
            return rsdp;
    }

    u32 base_mem_kb = (bda[0x14] << 8) | bda[0x13];
    if (base_mem_kb >= 1024)
    {
        struct acpi_rsdp* rsdp = acpi_rsdp_search(base_mem_kb * 1024 - 1024, 1024);
        if (rsdp)
            return rsdp;
    }

    return acpi_rsdp_search(0xE0000, 0x20000);
}

static int acpi_parse_madt(struct acpi_madt* madt)
{
    if (!madt || madt->header.length < sizeof(struct acpi_madt))
        return 0;

    lapic = (u32*)madt->lapic_addr;

    u8* p = (u8*)madt + sizeof(struct acpi_madt);
    u8* end = (u8*)madt + madt->header.length;
    while (p + sizeof(struct acpi_madt_entry) <= end)
    {
        struct acpi_madt_entry* entry = (struct acpi_madt_entry*)p;
        if (entry->length < sizeof(struct acpi_madt_entry) || p + entry->length > end)
            break;

        switch (entry->type)
        {
        case 0: // Processor Local APIC
            {
                struct acpi_madt_lapic* lapic_entry = (struct acpi_madt_lapic*)p;
                if (lapic_entry->flags & 0x01)
                    record_cpu_apicid(lapic_entry->apic_id);
                break;
            }
        case 1: // I/O APIC
            {
                struct acpi_madt_ioapic* ioapic_entry = (struct acpi_madt_ioapic*)p;
                ioapicid = ioapic_entry->ioapic_id;
                break;
            }
        case 5: // Local APIC address override
            {
                struct acpi_madt_lapic_override* override_entry = (struct acpi_madt_lapic_override*)p;
                lapic = (u32*)(u32)override_entry->lapic_addr;
                break;
            }
        case 9: // Processor Local x2APIC
            {
                struct acpi_madt_x2apic* x2apic_entry = (struct acpi_madt_x2apic*)p;
                if (x2apic_entry->flags & 0x01)
                    record_cpu_apicid(x2apic_entry->x2apic_id);
                break;
            }
        default:
            break;
        }

        p += entry->length;
    }

    return ncpu > 0 && lapic != nullptr;
}

static int acpi_visit_sdt(struct acpi_sdt_header* table, int entry_size)
{
    if (!table || table->length < sizeof(struct acpi_sdt_header))
        return 0;
    if (sum((u8*)table, table->length) != 0)
        return 0;

    int count = (table->length - sizeof(struct acpi_sdt_header)) / entry_size;
    u8* entries = (u8*)table + sizeof(struct acpi_sdt_header);

    for (int i = 0; i < count; i++)
    {
        unsigned long long addr = entry_size == 8
                                      ? ((unsigned long long*)entries)[i]
                                      : ((u32*)entries)[i];
        if (addr == 0)
            continue;
        if (entry_size == 8 && (addr >> 32) != 0)
            continue; // Ignore tables above 4GiB for now.
        if (addr >= PHYSTOP)
            continue; // Ignore tables beyond mapped physical memory.

        struct acpi_sdt_header* entry = (struct acpi_sdt_header*)P2V((u32)addr);
        if (memcmp(entry->signature, "APIC", 4) == 0)
        {
            if (sum((u8*)entry, entry->length) != 0)
                continue;
            if (acpi_parse_madt((struct acpi_madt*)entry))
                return 1;
        }
    }

    return 0;
}

static int acpi_init(void)
{
    struct acpi_rsdp* rsdp = acpi_find_rsdp();
    if (!rsdp)
        return 0;

    if (rsdp->rsdt_addr)
    {
        if (rsdp->rsdt_addr < PHYSTOP)
        {
            struct acpi_sdt_header* rsdt = (struct acpi_sdt_header*)P2V(rsdp->rsdt_addr);
            if (memcmp(rsdt->signature, "RSDT", 4) == 0 && acpi_visit_sdt(rsdt, 4))
                return lapic != nullptr && ncpu > 0;
        }
    }

    if (rsdp->revision >= 2)
    {
        struct acpi_rsdp_v2* rsdp2 = (struct acpi_rsdp_v2*)rsdp;
        if (rsdp2->xsdt_addr && (rsdp2->xsdt_addr >> 32) == 0)
        {
            if ((u32)rsdp2->xsdt_addr < PHYSTOP)
            {
                struct acpi_sdt_header* xsdt = (struct acpi_sdt_header*)P2V((u32)rsdp2->xsdt_addr);
                if (memcmp(xsdt->signature, "XSDT", 4) == 0 && acpi_visit_sdt(xsdt, 8))
                    return lapic != nullptr && ncpu > 0;
            }
        }
    }

    return 0;
}

void mpinit(void)
{
    ncpu = 0;
    lapic = nullptr;
    ioapicid = 0;

    int legacy = mpinit_legacy();
    int acpi = acpi_init();

    if (!legacy && !acpi)
        panic("Expect to run on an SMP");
}