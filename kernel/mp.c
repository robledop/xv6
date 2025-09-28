// Multiprocessor support
// Search memory for MP description structures.
// http://developer.intel.com/design/pentium/datashts/24201606.pdf

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mp.h"
#include "x86.h"
#include "mmu.h"
#include "proc.h"

struct cpu cpus[NCPU];
int ncpu;
uchar ioapicid;

static int mpinit_legacy(void);
static int acpiinit(void);

struct acpi_rsdp
{
    char signature[8];
    uchar checksum;
    char oemid[6];
    uchar revision;
    uint rsdt_addr;
} __attribute__((packed));

struct acpi_rsdp_v2
{
    struct acpi_rsdp v1;
    uint length;
    unsigned long long xsdt_addr;
    uchar extended_checksum;
    uchar reserved[3];
} __attribute__((packed));

struct acpi_sdt_header
{
    char signature[4];
    uint length;
    uchar revision;
    uchar checksum;
    char oemid[6];
    char oem_table_id[8];
    uint oem_revision;
    uint creator_id;
    uint creator_revision;
} __attribute__((packed));

struct acpi_madt
{
    struct acpi_sdt_header header;
    uint lapic_addr;
    uint flags;
} __attribute__((packed));

struct acpi_madt_entry
{
    uchar type;
    uchar length;
} __attribute__((packed));

struct acpi_madt_lapic
{
    struct acpi_madt_entry header;
    uchar acpi_processor_id;
    uchar apic_id;
    uint flags;
} __attribute__((packed));

struct acpi_madt_ioapic
{
    struct acpi_madt_entry header;
    uchar ioapic_id;
    uchar reserved;
    uint ioapic_addr;
    uint gsi_base;
} __attribute__((packed));

struct acpi_madt_lapic_override
{
    struct acpi_madt_entry header;
    ushort reserved;
    unsigned long long lapic_addr;
} __attribute__((packed));

struct acpi_madt_x2apic
{
    struct acpi_madt_entry header;
    ushort reserved;
    uint x2apic_id;
    uint flags;
    uint acpi_processor_uid;
} __attribute__((packed));

static uchar
sum(uchar* addr, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++)
        sum += addr[i];
    return sum;
}

// Look for an MP structure in the len bytes at addr.
static struct mp*
mpsearch1(uint a, int len)
{
    uchar* addr = P2V(a);
    uchar* e = addr + len;
    for (uchar* p = addr; p < e; p += sizeof(struct mp))
        if (memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct mp)) == 0)
            return (struct mp*)p;
    return 0;
}

// Search for the MP Floating Pointer Structure, which according to the
// spec is in one of the following three locations:
// 1) in the first KB of the EBDA;
// 2) in the last KB of system base memory;
// 3) in the BIOS ROM between 0xE0000 and 0xFFFFF.
static struct mp* mpsearch(void)
{
    uint p;
    struct mp* mp;

    uchar* bda = (uchar*)P2V(0x400);
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
static void record_cpu_apicid(uint apicid)
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

    if ((mp = mpsearch()) == 0 || mp->physaddr == 0)
        return 0;
    struct mpconf* conf = (struct mpconf*)P2V((uint) mp->physaddr);
    if (memcmp(conf, "PCMP", 4) != 0)
        return 0;
    if (conf->version != 1 && conf->version != 4)
        return 0;
    if (sum((uchar*)conf, conf->length) != 0)
        return 0;
    *pmp = mp;
    return conf;
}

static int mpinit_legacy(void)
{
    uchar *p, *e;
    struct mp* mp;
    struct mpconf* conf;

    if ((conf = mpconfig(&mp)) == 0)
        return 0;

    int ismp_ = 1;
    lapic = (uint*)conf->lapicaddr;
    for (p = (uchar*)(conf + 1), e = (uchar*)conf + conf->length; p < e;)
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

static struct acpi_rsdp* acpi_rsdp_search(uint phys_addr, int len)
{
    uchar* addr = P2V(phys_addr);
    uchar* end = addr + len;
    for (uchar* p = addr; p < end; p += 16)
        if (memcmp(p, "RSD PTR ", 8) == 0)
        {
            struct acpi_rsdp* rsdp = (struct acpi_rsdp*)p;
            uint length = sizeof(struct acpi_rsdp);
            if (rsdp->revision >= 2)
            {
                struct acpi_rsdp_v2* rsdp2 = (struct acpi_rsdp_v2*)p;
                if (rsdp2->length >= sizeof(struct acpi_rsdp))
                    length = rsdp2->length;
            }
            if (sum(p, length) == 0)
                return rsdp;
        }
    return 0;
}

static struct acpi_rsdp* acpi_find_rsdp(void)
{
    uchar* bda = (uchar*)P2V(0x400);
    uint ebda_segment = (bda[0x0F] << 8) | bda[0x0E];
    if (ebda_segment)
    {
        struct acpi_rsdp* rsdp = acpi_rsdp_search(ebda_segment << 4, 1024);
        if (rsdp)
            return rsdp;
    }

    uint base_mem_kb = (bda[0x14] << 8) | bda[0x13];
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

    lapic = (uint*)madt->lapic_addr;

    uchar* p = (uchar*)madt + sizeof(struct acpi_madt);
    uchar* end = (uchar*)madt + madt->header.length;
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
            lapic = (uint*)(uint)override_entry->lapic_addr;
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

    return ncpu > 0 && lapic != 0;
}

static int acpi_visit_sdt(struct acpi_sdt_header* table, int entry_size)
{
    if (!table || table->length < sizeof(struct acpi_sdt_header))
        return 0;
    if (sum((uchar*)table, table->length) != 0)
        return 0;

    int count = (table->length - sizeof(struct acpi_sdt_header)) / entry_size;
    uchar* entries = (uchar*)table + sizeof(struct acpi_sdt_header);

    for (int i = 0; i < count; i++)
    {
        unsigned long long addr = entry_size == 8 ? ((unsigned long long*)entries)[i]
                                                  : ((uint*)entries)[i];
        if (addr == 0)
            continue;
        if (entry_size == 8 && (addr >> 32) != 0)
            continue; // Ignore tables above 4GiB for now.
        if (addr >= PHYSTOP)
            continue; // Ignore tables beyond mapped physical memory.

        struct acpi_sdt_header* entry = (struct acpi_sdt_header*)P2V((uint)addr);
        if (memcmp(entry->signature, "APIC", 4) == 0)
        {
            if (sum((uchar*)entry, entry->length) != 0)
                continue;
            if (acpi_parse_madt((struct acpi_madt*)entry))
                return 1;
        }
    }

    return 0;
}

static int acpiinit(void)
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
                return lapic != 0 && ncpu > 0;
        }
    }

    if (rsdp->revision >= 2)
    {
        struct acpi_rsdp_v2* rsdp2 = (struct acpi_rsdp_v2*)rsdp;
        if (rsdp2->xsdt_addr && (rsdp2->xsdt_addr >> 32) == 0)
        {
            if ((uint)rsdp2->xsdt_addr < PHYSTOP)
            {
                struct acpi_sdt_header* xsdt = (struct acpi_sdt_header*)P2V((uint)rsdp2->xsdt_addr);
                if (memcmp(xsdt->signature, "XSDT", 4) == 0 && acpi_visit_sdt(xsdt, 8))
                    return lapic != 0 && ncpu > 0;
            }
        }
    }

    return 0;
}

void mpinit(void)
{
    ncpu = 0;
    lapic = 0;
    ioapicid = 0;

    int legacy = mpinit_legacy();
    int acpi = acpiinit();

    if (!legacy && !acpi)
        panic("Expect to run on an SMP");
}
