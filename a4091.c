/*
 * A4091  Version 0.4 2022-07-24
 * -----------------------------
 *
 * Utility to inspect and test an installed A4091 SCSI controller card
 * for correct operation.
 *
 * Copyright 2022 Chris Hooper.  This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written or email approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
const char *version = "\0$VER: A4091 0.4 ("__DATE__") � Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <libraries/expansionbase.h>
#include <clib/expansion_protos.h>
#include <inline/exec.h>
#include <inline/expansion.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <sys/time.h>

/* ULONG has changed from NDK 3.9 to NDK 3.2.
 * However, PRI*32 did not. What is the right way to implement this?
 */
#if INCLUDE_VERSION < 47
#undef PRIx32
#define PRIx32 "lx"
#endif

#define CACHE_LINE_WRITE(addr, len) CacheClearE((void *)(addr), len, \
                                                CACRF_ClearD)
#define CACHE_LINE_DISCARD(addr, len) CacheClearE((void *)(addr), len, \
                                                  CACRF_InvalidateD)

#define A4091_OFFSET_AUTOCONFIG 0x00000000
#define A4091_OFFSET_ROM        0x00000000
#define A4091_OFFSET_REGISTERS  0x00800000
#define A4091_OFFSET_SWITCHES   0x008c0003

#define ZORRO_MFG_COMMODORE     0x0202
#define ZORRO_PROD_A4091        0x0054

#define A4091_INTPRI 30
#define A4091_IRQ    3

extern struct ExecBase *SysBase;

#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile uint32_t *)(x)

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))
#define BIT(x)        (1 << (x))

#define FLAG_DEBUG            0x01        /* Debug output */
#define FLAG_MORE_DEBUG       0x02        /* More debug output */

#define SUPERVISOR_STATE_ENTER() { \
                                   APTR old_stack = SuperState()
#define SUPERVISOR_STATE_EXIT()    UserState(old_stack); \
                                 }
#define INTERRUPTS_DISABLE() Disable()  /* Disable Interrupts */
#define INTERRUPTS_ENABLE()  Enable()   /* Enable Interrupts */

#define FIRST(n)   (struct Interrupt *)((n)->lh_Head)
#define EMPTY(n)   ((n)->lh_Head == (struct Node *)&n->lh_Tail)
#define NEXTINT(i) (struct Interrupt *)((i)->is_Node.ln_Succ)
#define LAST(i)    (((i)->is_Node.ln_Succ)->ln_Succ == 0)


/* NCR53C710 registers */
#define REG_SCNTL0  0x03  // SCSI control 0
#define REG_SCNTL1  0x02  // SCSI control 1
#define REG_SDID    0x01  // SCSI destination ID
#define REG_SIEN    0x00  // SCSI interrupt enable
#define REG_SCID    0x07  // SCSI chip ID
#define REG_SCFER   0x06  // SCSI transfer
#define REG_SODL    0x05  // SCSI output data latch
#define REG_SOCL    0x04  // SCSI output control latch
#define REG_SFBR    0x0b  // SCSI first byte received
#define REG_SIDL    0x0a  // SCSI input data latch
#define REG_SBDL    0x09  // SCSI bus data lines
#define REG_SBCL    0x08  // SCSI bus control lines
#define REG_DSTAT   0x0f  // DMA status
#define REG_SSTAT0  0x0e  // SCSI status 0
#define REG_SSTAT1  0x0d  // SCSI status 1
#define REG_SSTAT2  0x0c  // SCSI status 2
#define REG_DSA     0x10  // Data structure address
#define REG_CTEST0  0x17  // Chip test 0
#define REG_CTEST1  0x16  // Chip test 1
#define REG_CTEST2  0x15  // Chip test 2
#define REG_CTEST3  0x14  // Chip test 3
#define REG_CTEST4  0x1b  // Chip test 4: MUX ZMOD SZM SLBE SFWR FBL2-FBL0
#define REG_CTEST5  0x1a  // Chip test 5
#define REG_CTEST6  0x19  // Chip test 6: DMA FIFO
#define REG_CTEST7  0x18  // Chip test 7
#define REG_TEMP    0x1c  // Temporary stack
#define REG_DFIFO   0x23  // DMA FIFO
#define REG_ISTAT   0x22  // Interrupt status
#define REG_CTEST8  0x21  // Chip test 8
#define REG_LCRC    0x20  // Longitudinal parity
#define REG_DBC     0x25  // DMA byte counter
#define REG_DCMD    0x24  // DMA command
#define REG_DNAD    0x28  // DMA next address for data
#define REG_DSP     0x2c  // DMA SCRIPTS pointer
#define REG_DSPS    0x30  // DMA SCRIPTS pointer save
#define REG_SCRATCH 0x34  // General purpose scratch pad
#define REG_DMODE   0x3b  // DMA mode
#define REG_DIEN    0x3a  // DMA interrupt enable
#define REG_DWT     0x39  // DMA watchdog timer
#define REG_DCNTL   0x38  // DMA control
#define REG_ADDER   0x3c  // Sum output of internal adder

#define REG_SCNTL0_EPG  BIT(2)  // Generate parity on the SCSI bus

#define REG_SIEN_PAR    BIT(0)  // Interrupt on parity error
#define REG_SIEN_RST    BIT(1)  // Interrupt on SCSI reset received
#define REG_SIEN_UDC    BIT(2)  // Interrupt on Unexpected disconnect
#define REG_SIEN_SGE    BIT(3)  // Interrupt on SCSI gross error
#define REG_SIEN_SEL    BIT(4)  // Interrupt on Selected or reselected
#define REG_SIEN_STO    BIT(5)  // Interrupt on SCSI bus timeout
#define REG_SIEN_FCMP   BIT(6)  // Interrupt on Function complete
#define REG_SIEN_PM     BIT(7)  // Interrupt on Unexpected Phase mismatch

#define REG_DIEN_BF     BIT(5)  // DMA interrupt on Bus Fault
#define REG_DIEN_ABRT   BIT(4)  // DMA interrupt on Aborted
#define REG_DIEN_SSI    BIT(3)  // DMA interrupt on SCRIPT Step Interrupt
#define REG_DIEN_SIR    BIT(2)  // DMA interrupt on SCRIPT Interrupt Instruction
#define REG_DIEN_WTD    BIT(1)  // DMA interrupt on Watchdog Timeout Detected
#define REG_DIEN_ILD    BIT(0)  // DMA interrupt on Illegal Instruction Detected

#define REG_ISTAT_DIP   BIT(0)  // DMA interrupt pending
#define REG_ISTAT_SIP   BIT(1)  // SCSI interrupt pending
#define REG_ISTAT_RST   BIT(6)  // Reset the 53C710
#define REG_ISTAT_ABRT  BIT(7)  // Abort

#define REG_DMODE_MAN   BIT(0)  // DMA Manual start mode
#define REG_DMODE_U0    BIT(1)  // DMA User programmable transfer type
#define REG_DMODE_FAM   BIT(2)  // DMA Fixed Address mode (set avoids DNAD inc)
#define REG_DMODE_PD    BIT(3)  // When set: FC0=0 for data & FC0=1 for program
#define REG_DMODE_FC1   BIT(4)  // Value driven on FC1 when bus mastering
#define REG_DMODE_FC2   BIT(5)  // Value driven on FC2 when bus mastering
#define REG_DMODE_BLE0  0                  // Burst length 1-transfer
#define REG_DMODE_BLE1  BIT(6)             // Burst length 2-transfers
#define REG_DMODE_BLE2  BIT(7)             // Burst length 4-transfers
#define REG_DMODE_BLE3  (BIT(6) | BIT(7))  // Burst length 8-transfers

#define REG_DCNTL_COM   BIT(0)  // Enable 53C710 mode
#define REG_DCNTL_STD   BIT(2)  // Start DMA operation (execute SCRIPT)
#define REG_DCNTL_LLM   BIT(3)  // Low level modfe (no DMA or SCRIPTS)
#define REG_DCNTL_SSM   BIT(4)  // SCRIPTS single-step mode
#define REG_DCNTL_EA    BIT(5)  // Enable Ack
#define REG_DCNTL_CFD0  BIT(7)             // SCLK 16.67-25.00 MHz
#define REG_DCNTL_CFD1  BIT(6)             // SCLK 25.01-37.50 MHz
#define REG_DCNTL_CFD2  0                  // SCLK 37.50-50.00 MHz
#define REG_DCNTL_CFD3  (BIT(7) | BIT(6))  // SCLK 50.01-66.67 MHz

#define REG_DSTAT_SSI   BIT(3)  // SCRIPTS single-step interrupt
#define REG_DSTAT_ABRT  BIT(4)  // SCRIPTS single-step interrupt
#define REG_DSTAT_DFE   BIT(7)  // DMA FIFO empty

#define REG_SCNTL1_ASEP BIT(2)  // Assert even SCSI data partity
#define REG_SCNTL1_RST  BIT(3)  // Assert reset on SCSI bus
#define REG_SCNTL1_ADB  BIT(6)  // Assert SCSI data bus (SODL/SOCL registers)

#define REG_SSTAT1_PAR  BIT(0)  // SCSI parity state
#define REG_SSTAT1_RST  BIT(1)  // SCSI bus reset is asserted

#define REG_CTEST4_FBL2 BIT(2)  // Send CTEST6 register to lane of the DMA FIFO
#define REG_CTEST4_SLBE BIT(4)  // SCSI loopback mode enable
#define REG_CTEST4_CDIS BIT(7)  // Cache burst disable

#define REG_CTEST5_DACK BIT(0)  // Data acknowledge (1=DMA acks SCSI DMA req)
#define REG_CTEST5_DREQ BIT(1)  // Data request (1=SCSI requests DMA transfer)
#define REG_CTEST5_DDIR BIT(3)  // DMA direction (1=SCSI->host, 0=host->SCSI)

#define REG_CTEST8_CLF  BIT(2)  // Clear DMA and SCSI FIFOs
#define REG_CTEST8_FLF  BIT(3)  // Flush DMA FIFO

#define NCR_FIFO_SIZE 16

/* Modern stdint types */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef unsigned int   uint;

static uint               runtime_flags = 0;
static const char * const expansion_library_name = "expansion.library";

typedef struct {
    uint32_t addr;
    uint32_t intcount;                   // Total interrupts
    uint8_t  card_owned;
    uint8_t  cleanup_installed;
    uint8_t  reg_dcntl;
    uint8_t  reg_istat;
    uint32_t reg_00;              // SIEN SDID SCNTL1 SCNTL0
    uint32_t reg_04;              // SOCL SODL SXFER SCID
    uint32_t reg_08;              // SBCL SBDL SIDL SFBR
    uint32_t reg_10;              // DSA
    uint32_t reg_1c;              // TEMP
    uint32_t reg_24;              // DCMD DBC
    uint32_t reg_28;              // DNAD
    uint32_t reg_2c;              // DSP
    uint32_t reg_30;              // DSPS
    uint32_t reg_34;              // SCRATCH
    uint32_t reg_38;              // DNCTL DWT DIEN DMODE
    uint32_t reg_3c;              // ADDER
    struct Interrupt *local_isr;  // Temporary interrupt server
    struct Interrupt *driver_isr; // SCSI driver interrupt server
    volatile uint8_t ireg_istat;  // ISTAT captured by interrupt handler
    volatile uint8_t ireg_sien;   // SIEN captured by interrupt handler
    volatile uint8_t ireg_sstat0; // SSTAT0 captured by interrupt handler
    volatile uint8_t ireg_dstat;  // DSTAT captured by interrupt handler
} a4091_save_t;

static a4091_save_t a4091_save;

BOOL __check_abort_enabled = 0;

static void
check_break(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
        printf("^C Abort\n");
        exit(1);
    }
}

static uint64_t
read_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    return ((uint64_t) (ds.ds_Days * 24 * 60 +
                        ds.ds_Minute) * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

static const char * const z2_config_sizes[] =
{
    "8 MB", "64 KB", "128 KB", "256 KB", "512KB", "1MB", "2MB", "4MB"
};

static const char * const z3_config_sizes[] =
{
    "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "RSVD"
};

static const char * const config_subsizes[] =
{
    "Same-as-Physical", "Automatically-sized", "64 KB", "128 KB",
    "256 KB", "512 KB", "1MB", "2MB",
    "4MB", "6MB", "8MB", "10MB", "12MB", "14MB", "Rsvd1", "Rsvd2"
};

static uint32_t a4091_base;  // Base address for current card

static uint8_t
get_creg(uint reg)
{
    uint8_t hi = ~*ADDR8(a4091_base + A4091_OFFSET_AUTOCONFIG + reg);
    uint8_t lo = ~*ADDR8(a4091_base + A4091_OFFSET_AUTOCONFIG + reg + 0x100);
    return ((hi & 0xf0) | (lo >> 4));
}

#if 0
static void
set_creg(uint32_t addr, uint reg, uint8_t value)
{
    *ADDR8(addr + A4091_OFFSET_AUTOCONFIG + reg) = value;
    *ADDR8(addr + A4091_OFFSET_AUTOCONFIG + reg + 0x100) = value << 4;
}
#endif

static void
show_creg_value(uint reg, uint8_t value)
{
    printf("   %02x   %02x", reg, value);
}

static uint8_t
show_creg(uint reg)
{
    uint8_t value = get_creg(reg);
    show_creg_value(reg, value);
    return (value);
}

static int
autoconfig_reserved(uint reg)
{
    uint8_t value = get_creg(reg);
    if (value != 0x00) {
        show_creg_value(reg, value);
        printf(" Reserved: should be 0x00\n");
        return (1);
    }
    return (0);
}


static int
decode_autoconfig(void)
{
    uint8_t  value;
    uint32_t value32;
    int      rc = 0;
    int      is_z3 = 0;
    int      is_autoboot = 0;
    int      byte;
    const char * const *sizes = z2_config_sizes;

    printf("A4091 Autoconfig area\n"
           "  Reg Data Decode\n");
    value = ~show_creg(0x00);
    switch (value >> 6) {
        case 0:
        case 1:
            printf(" Zorro_Reserved");
            break;
        case 2:
            printf(" ZorroIII");
            is_z3 = 1;
            break;
        case 3:
            printf(" ZorroII");
            break;
    }
    if (value & BIT(5))
        printf(" Memory");
    if (is_z3 && (get_creg(0x08) & BIT(5)))
        sizes = z3_config_sizes;
    printf(" Size=%s", sizes[value & 0x7]);
    if (value & BIT(4)) {
        printf(" Autoboot");
        is_autoboot = 1;
    }
    if (value & BIT(3))
        printf(" Link-to-next");
    printf("\n");

    printf(" Product=0x%02x\n", show_creg(0x04) & 0xff);

    rc += autoconfig_reserved(0x0c);

    value = show_creg(0x08);
    if (is_z3) {
        if (value & BIT(7)) {
            printf(" Device-Memory");
            rc++;  // Unexpected for A4091
        } else {
            printf(" Device-IO");
        }
    } else {
        rc++;  // Unexpected for A4091
        if (value & BIT(7))
            printf(" Fit-ZorroII");
        else
            printf(" Fit-anywhere");
    }
    if (value & BIT(5))
        printf(" NoShutup");
    if (is_z3 && ((value & BIT(4)) == 0))
        printf(" Invalid_RSVD");

    if (value & BIT(5))
        printf(" SizeExt");
    printf(" %s\n", config_subsizes[value & 0x0f]);


    if (autoconfig_reserved(0x0c))
        rc = 1;

    value32 = show_creg(0x10) << 8;
    printf(" Mfg Number high byte\n");
    value32 |= show_creg(0x14);
    printf(" Mfg Number low byte    Manufacturer=0x%04x\n", value32);

    value32 = 0;
    for (byte = 0; byte < 4; byte++) {
        value32 <<= 8;
        value32 |= show_creg(0x18 + byte * 4);
        printf(" Serial number byte %d", byte);
        if (byte == 3)
            printf("   Serial=0x%08x", value32);
        printf("\n");
    }

    if (is_autoboot) {
        value32 = show_creg(0x28) << 8;
        printf(" Option ROM vector high\n");
        value32 |= show_creg(0x2c);
        printf(" Option ROM vector low  Offset=0x%04x\n", value32);
    }
    for (byte = 0x30; byte <= 0x3c; byte += 4)
        rc += autoconfig_reserved(byte);
    for (byte = 0x52; byte <= 0x7c; byte += 4)
        rc += autoconfig_reserved(byte);

    return (rc);
}

typedef const char * const bitdesc_t;

static bitdesc_t bits_scntl0[] = {
    "TRG", "AAP", "EPG", "EPC", "WATN/", "START", "ARB0", "ARB1"
};
static bitdesc_t bits_scntl1[] = {
    "RES0", "RES1", "AESP", "RST", "CON", "FSR", "ADB", "EXC"
};
static bitdesc_t bits_sien[] = {
    "PAR", "RST/", "UDC", "SGE", "SEL", "STO", "FCMP", "M/A"
};
static bitdesc_t bits_sbcl[] = {
    "I/O", "C/D", "MSG", "ATN", "SEL", "BSY", "ACK", "REQ"
};
static bitdesc_t bits_dstat[] = {
    "IID", "WTD", "SIR", "SSI", "ABRT", "RF", "RES6", "DFE"
};
static bitdesc_t bits_sstat0[] = {
    "PAR", "RST/", "UDC", "SGE", "SEL", "STO", "FCMP", "M/A"
};
static bitdesc_t bits_sstat1[] = {
    "SDP/", "RST/", "WOA", "LOA", "AIP", "OLF", "ORF", "ILF"
};
static bitdesc_t bits_sstat2[] = {
    "I/O", "C/D", "MSG", "SDP", "FF0", "FF1", "FF2", "FF3"
};
static bitdesc_t bits_ctest0[] = {
    "DDIR", "RES1", "ERF", "HSC", "EAN", "GRP", "BTD", "RES7"
};
static bitdesc_t bits_ctest2[] = {
    "DACK", "DREQ", "TEOP", "DFP", "SFP", "SOFF", "SIGP", "RES7"
};
static bitdesc_t bits_ctest4[] = {
    "FBL0", "FBL1", "FBL2", "SFWR", "SLBE", "SZM", "ZMOD", "MUX"
};
static bitdesc_t bits_ctest5[] = {
    "DACK", "DREQ", "EOP", "DDIR", "MASR", "ROFF", "BBCK", "ADCK"
};
static bitdesc_t bits_ctest7[] = {
    "DIFF", "TT1", "EVP", "DFP", "NOTIME", "SC0", "SC1", "CDIS"
};
static bitdesc_t bits_istat[] = {
    "DIP", "SIP", "RSV2", "CON", "RSV4", "SIOP", "RST", "ABRT"
};
static bitdesc_t bits_ctest8[] = {
    "SM", "FM", "CLF", "FLF", "V0", "V1", "V2", "V3"
};
static bitdesc_t bits_dmode[] = {
    "MAN", "U0", "FAM", "PD", "FC1", "FC2", "BL0", "BL1"
};
static bitdesc_t bits_dien[] = {
    "HD", "WTD", "SIR", "SSI", "ABRT", "BF", "RES6", "RES7"
};
static bitdesc_t bits_dcntl[] = {
    "COM", "FA", "STD", "LLM", "SSM", "EA", "CF0", "CF1"
};

typedef struct {
    uint8_t            reg_loc;
    uint8_t            reg_size;     // size in bytes
    uint8_t            show;         // safe to read/display this register
    uint8_t            pad;          // structure padding
    const char         reg_name[8];  // Register name
    const char * const reg_desc;     // Long description
    bitdesc_t         *reg_bits;     // Individual bit names
} ncr_regdefs_t;

static const ncr_regdefs_t ncr_regdefs[] =
{
    { 0x03, 1, 1, 0, "SCNTL0",   "SCSI control 0", bits_scntl0 },
    { 0x02, 1, 1, 0, "SCNTL1",   "SCSI control 1", bits_scntl1 },
    { 0x01, 1, 1, 0, "SDID",     "SCSI destination ID" },
    { 0x00, 1, 1, 0, "SIEN",     "SCSI IRQ enable", bits_sien },
    { 0x07, 1, 1, 0, "SCID",     "SCSI chip ID" },
    { 0x06, 1, 1, 0, "SXFER",    "SCSI transfer" },
    { 0x05, 1, 1, 0, "SODL",     "SCSI output data latch" },
    { 0x04, 1, 1, 0, "SOCL",     "SCSI output control latch", bits_sbcl },
    { 0x0b, 1, 1, 0, "SFBR",     "SCSI first byte received" },
    { 0x0a, 1, 1, 0, "SIDL",     "SCSI input data latch" },
    { 0x09, 1, 1, 0, "SBDL",     "SCSI bus data lines" },
    { 0x08, 1, 1, 0, "SBCL",     "SCSI bus contol lines", bits_sbcl },
    { 0x0f, 1, 1, 0, "DSTAT",    "DMA status", bits_dstat },
    { 0x0e, 1, 1, 0, "SSTAT0",   "SCSI status 0", bits_sstat0 },
    { 0x0d, 1, 1, 0, "SSTAT1",   "SCSI status 1", bits_sstat1 },
    { 0x0c, 1, 1, 0, "SSTAT2",   "SCSI status 2", bits_sstat2 },
    { 0x10, 4, 1, 0, "DSA",      "Data structure address" },
    { 0x17, 1, 1, 0, "CTEST0",   "Chip test 0", bits_ctest0 },
    { 0x16, 1, 1, 0, "CTEST1",   "Chip test 1 7-4=FIFO_Empty 3-0=FIFO_Full" },
    { 0x15, 1, 1, 0, "CTEST2",   "Chip test 2", bits_ctest2 },
    { 0x14, 1, 0, 0, "CTEST3",   "Chip test 3 SCSI FIFO" },
    { 0x1b, 1, 1, 0, "CTEST4",   "Chip test 4", bits_ctest4 },
    { 0x1a, 1, 1, 0, "CTEST5",   "Chip test 5", bits_ctest5 },
    { 0x19, 1, 0, 0, "CTEST6",   "Chip test 6 DMA FIFO" },
    { 0x18, 1, 1, 0, "CTEST7",   "Chip test 7", bits_ctest7 },
    { 0x1c, 4, 1, 0, "TEMP",     "Temporary Stack" },
    { 0x23, 1, 1, 0, "DFIFO",    "DMA FIFO" },
    { 0x22, 1, 1, 0, "ISTAT",    "Interrupt Status", bits_istat },
    { 0x21, 1, 1, 0, "CTEST8",   "Chip test 8", bits_ctest8 },
    { 0x20, 1, 1, 0, "LCRC",     "Longitudinal parity" },
    { 0x25, 3, 1, 0, "DBC",      "DMA byte counter" },
    { 0x24, 1, 1, 0, "DCMD",     "DMA command" },
    { 0x28, 4, 1, 0, "DNAD",     "DMA next address for data" },
    { 0x2c, 4, 1, 0, "DSP",      "DMA SCRIPTS pointer" },
    { 0x30, 4, 1, 0, "DSPS",     "DMA SCRIPTS pointer save" },
    { 0x34, 4, 1, 0, "SCRATCH",  "General purpose scratch pad" },
    { 0x3b, 1, 1, 0, "DMODE",    "DMA mode", bits_dmode },
    { 0x3a, 1, 1, 0, "DIEN",     "DMA interrupt enable", bits_dien },
    { 0x39, 1, 1, 0, "DWT",      "DMA watchdog timer" }, // No support in FS-UAE
    { 0x38, 1, 1, 0, "DCNTL",    "DMA control", bits_dcntl },
    { 0x3c, 4, 1, 0, "ADDER",    "Sum output of internal adder" },
};

static uint8_t
get_ncrreg8_noglob(uint32_t a4091_base, uint reg)
{
    return (*ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg));
}

static uint8_t
get_ncrreg8(uint reg)
{
    return (*ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg));
}

static uint32_t
get_ncrreg32(uint reg)
{
    return (*ADDR32(a4091_base + A4091_OFFSET_REGISTERS + reg));
}

/* Write at shadow register (+0x40) to avoid 68030 write-allocate bug */
static void
set_ncrreg8(uint reg, uint8_t value)
{
    *ADDR8(a4091_base + A4091_OFFSET_REGISTERS + 0x40 + reg) = value;
}

static void
set_ncrreg32(uint reg, uint32_t value)
{
    *ADDR32(a4091_base + A4091_OFFSET_REGISTERS + 0x40 + reg) = value;
}

#if 0
static void
flush_ncrreg32(uint reg)
{
    CacheClearE((void *)(a4091_base + A4091_OFFSET_REGISTERS + reg), 4,
                CACRF_ClearD);
}
#endif

/*
 * access_timeout
 * --------------
 * Returns non-zero if the number of ticks has elapsed since the specified
 * tick_start.
 */
static int
access_timeout(const char *msg, uint32_t ticks, uint64_t tick_start)
{
    uint64_t tick_end = read_system_ticks();

    if (tick_end < tick_start) {
        printf("Invalid time comparison: %08x:%08x < %08x:%08x\n",
               (uint32_t) (tick_end >> 32), (uint32_t) tick_end,
               (uint32_t) (tick_start >> 32), (uint32_t) tick_start);
        return (FALSE);  /* Should not occur */
    }

    /* Signed integer compare to avoid wrap */
    if ((int) (tick_end - tick_start) > (int) ticks) {
        uint64_t diff;
        printf("%s: %d ticks", msg, (uint32_t) (tick_end - tick_start));
        diff = tick_end - tick_start;
        if (diff > TICKS_PER_SECOND * 10) {
            printf(": bug? %08x%08x %08x%08x\n",
                   (uint32_t) (tick_end >> 32), (uint32_t) tick_start,
                   (uint32_t) (tick_end >> 32), (uint32_t) tick_end);
        }
        printf("\n");
        return (TRUE);
    }
    return (FALSE);
}

/*
 * a4091_reset
 * -----------
 * Resets the A4091's 53C710 SCSI controller.
 */
static void
a4091_reset(void)
{
    set_ncrreg8(REG_DCNTL, REG_DCNTL_EA);   // Enable Ack: allow register writes
    set_ncrreg8(REG_ISTAT, REG_ISTAT_RST);  // Reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write

    set_ncrreg8(REG_ISTAT, 0);              // Clear reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write

    set_ncrreg8(REG_SCID, BIT(7));          // Set SCSI ID
    set_ncrreg8(REG_DCNTL, REG_DCNTL_EA);   // SCSI Core clock (37.51-50 MHz)

    set_ncrreg8(REG_DWT, 0xff);             // 25MHz DMA timeout: 640ns * 0xff
#if 0
    /* Set DMA interrupt enable on Bus Fault, Abort, or Illegal instruction */
    set_ncrreg8(REG_DIEN, REG_DIEN_BF | REG_DIEN_ABRT | REG_DIEN_ILD);
#endif

    // Reset Enable Acknowlege and Function Control One (FC1) bits?
}

/*
 * a4091_abort
 * -----------
 * Abort the current SCRIPTS operation, stopping the SCRIPTS processor.
 */
static void
a4091_abort(void)
{
    uint8_t istat;
    uint64_t tick_start;

    istat = get_ncrreg8(REG_ISTAT);
    set_ncrreg8(REG_ISTAT, istat | REG_ISTAT_ABRT);
    (void) get_ncrreg8(REG_ISTAT);

    tick_start = read_system_ticks();
    while ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_ABRT) == 0) {
        if (access_timeout("DSTAT_ABRT timeout", 2, tick_start))
            break;
    }
}

/*
 * a4091_irq_handler
 * -----------------
 * Handle interupts from the 53C710 SCSI controller
 */
LONG
a4091_irq_handler(void)
{
    register a4091_save_t *save asm("a1");

    uint8_t       istat = get_ncrreg8_noglob(save->addr, REG_ISTAT);

    if ((istat & (REG_ISTAT_DIP | REG_ISTAT_SIP)) != 0) {
        /*
         * If ISTAT_SIP is set, read SSTAT0 register to determine cause
         * If ISTAT_DIP is set, read DSTAT register to determine cause
         */
        save->ireg_istat  = istat;
        save->ireg_sien   = get_ncrreg8_noglob(save->addr, REG_SIEN);
        save->ireg_sstat0 = get_ncrreg8_noglob(save->addr, REG_SSTAT0);
        save->ireg_dstat  = get_ncrreg8_noglob(save->addr, REG_DSTAT);

        save->intcount++;

        if (save->intcount == 1)
            return (1);  // Handled
//      return (1);  // Handled
    }

    return (0);  // Not handled
}

static void
a4091_add_local_irq_handler(void)
{
    a4091_save.intcount = 0;
    a4091_save.local_isr = AllocMem(sizeof (*a4091_save.local_isr),
                                    MEMF_CLEAR | MEMF_PUBLIC);
    a4091_save.local_isr->is_Node.ln_Type = NT_INTERRUPT;
    a4091_save.local_isr->is_Node.ln_Pri  = A4091_INTPRI;
    a4091_save.local_isr->is_Node.ln_Name = "A4091 test";
    a4091_save.local_isr->is_Data         = &a4091_save;
    a4091_save.local_isr->is_Code         = (VOID (*)()) a4091_irq_handler;

    if (runtime_flags & FLAG_DEBUG)
        printf("my irq handler=%x %x\n",
               (uint32_t) &a4091_save, (uint32_t) a4091_save.local_isr);
    AddIntServer(A4091_IRQ, a4091_save.local_isr);
}

static void
a4091_remove_local_irq_handler(void)
{
    if (a4091_save.local_isr != 0) {
        RemIntServer(A4091_IRQ, a4091_save.local_isr);
        FreeMem(a4091_save.local_isr, sizeof (*a4091_save.local_isr));
        a4091_save.local_isr = 0;
    }
}

static char *
GetNodeName(struct Node *node)
{
    if (node == NULL)
        return ("");
    return ((node->ln_Name == NULL) ? "(missing)" : node->ln_Name);
}

static void
a4091_disable_driver_irq_handler(int verbose)
{
    struct IntVector *iv    = &SysBase->IntVects[A4091_IRQ];
    struct List      *slist = iv->iv_Data;
    struct Interrupt *s;
    int suspended = 0;

    if (EMPTY(slist))
        return;

    Disable();
    for (s = FIRST(slist); NEXTINT(s); s = NEXTINT(s)) {
        const char *name = GetNodeName((struct Node *) s);
        if (runtime_flags & FLAG_DEBUG) {
            Enable();
            printf("  %08x %08x %08x %s\n",
                   (uint32_t) s->is_Code, (uint32_t) s->is_Data,
                   (uint32_t) &s->is_Node, name);
            Disable();
        }
        if (strcmp(name, "NCR SCSI") == 0) {
            /* Found A4091 SCSI driver's interrupt server -- disable it */
            suspended = 1;
            a4091_save.driver_isr = (struct Interrupt *) &s->is_Node;
            RemIntServer(A4091_IRQ, a4091_save.driver_isr);
        }
    }
    Enable();
    if (suspended &&
        ((runtime_flags & FLAG_DEBUG) || verbose)) {
        printf("Suspended NCR SCSI driver IRQ handler\n");
    }
}

static void
a4091_enable_driver_irq_handler(void)
{
    if (a4091_save.driver_isr != 0) {
        if (runtime_flags & FLAG_DEBUG)
            printf("Restoring NCR SCSI driver IRQ handler\n");
        AddIntServer(A4091_IRQ, a4091_save.driver_isr );
        a4091_save.driver_isr = 0;
    }
}

static void
a4091_disable_handler_process(void)
{
    struct Node *node;
    int removed = 0;
    Forbid();
    for (node = SysBase->TaskReady.lh_Head;
         node->ln_Succ != NULL; node = node->ln_Succ) {
        const char *name = GetNodeName((struct Node *) node);
        if (strcmp(name, "A3090 SCSI handler") == 0) {
            Remove(node);
            removed++;
            goto done;
        }
    }
    for (node = SysBase->TaskWait.lh_Head;
         node->ln_Succ != NULL; node = node->ln_Succ) {
        const char *name = GetNodeName((struct Node *) node);
        if (strcmp(name, "A3090 SCSI handler") == 0) {
            Remove(node);
            removed++;
            goto done;
        }
    }
done:
    Permit();
    if (removed) {
        printf("Removed A3090 SCSI handler process\n");
    }
}

static void
a4091_remove_driver_from_devlist(void)
{
    /*
     * XXX: I've not found a reliable way to locate the "correct" device
     *      to remove from the devlist.
     *
     *      devlist entries don't have an association with the physical
     *      device being handled.
     *
     *      The configdevlist doesn't appear to have a valid a pointer
     *      to the specific driver in use (at least in my A3000 with 3.2.1,
     *      I got 0x00000000 for cd_Driver).
     */
#if 0
    struct Node *node;
    struct Node *next;

    Forbid();
    /* Note - printing within Forbid() would break the forbidden state */
    for (node = SysBase->DeviceList.lh_Head;
         node->ln_Succ != NULL; node = next) {
        const char *name = GetNodeName((struct Node *) node);
        next = node->ln_Succ;
        if (strstr(name, "scsi.device") != NULL) {
            struct Library *lib = (struct Library *) node;
            printf("found %p %s %s\n", node, node->ln_Name, (char *)lib->lib_IdString);
        }
    }
    Permit();


    struct Library   *ExpansionBase;
    struct ConfigDev *cdev  = NULL;
    uint32_t          addr  = -1;  /* Default to not found */
    int               count = 0;
    uint32_t pos = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return;
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            if (pos == count) {
                addr = (uint32_t) cdev->cd_BoardAddr;
                printf("found %08x %p %p\n", addr, cdev, cdev->cd_Driver);
                break;
            }
            count++;
        }
    } while (cdev != NULL);

    CloseLibrary(ExpansionBase);
#endif
}

static int
kill_driver(void)
{
    /* XXX: Should first unmount filesystems served by the driver */
    a4091_reset();
    a4091_disable_driver_irq_handler(1);
    a4091_disable_handler_process();
    a4091_remove_driver_from_devlist();
    return (0);
}

/*
 * a4091_state_restore
 * -------------------
 * Resets the A4091's 53C710, restores A4091 state, and then disables the
 * private interrupt handler.
 */
static void
a4091_state_restore(void)
{
    if (a4091_save.card_owned) {
        a4091_save.card_owned = 0;
        a4091_reset();
        a4091_enable_driver_irq_handler();
        a4091_remove_local_irq_handler();

#if 0
        set_ncrreg32(0x00, a4091_save.reg_00); // SIEN SDID SCNTL1 SCNTL0
        set_ncrreg32(0x04, a4091_save.reg_04); // SOCL SODL SXFER SCID
        set_ncrreg32(0x08, a4091_save.reg_08); // SBCL SBDL SIDL SFBR
        set_ncrreg32(0x10, a4091_save.reg_10); // DSA
        set_ncrreg32(0x1c, a4091_save.reg_1c); // TEMP
        set_ncrreg32(0x24, a4091_save.reg_24); // DCMD DBC
        set_ncrreg32(0x28, a4091_save.reg_28); // DNAD
        set_ncrreg32(0x2c, a4091_save.reg_2c); // DSP
        set_ncrreg32(0x30, a4091_save.reg_30); // DSPS
        set_ncrreg32(0x34, a4091_save.reg_34); // SCRATCH
        set_ncrreg32(0x3c, a4091_save.reg_3c); // ADDER
        set_ncrreg32(0x38, a4091_save.reg_38); // DNCTL DWT DIEN DMODE
#endif

        if ((a4091_save.intcount != 0) & (runtime_flags & FLAG_DEBUG)) {
            printf("Interrupt count=%d "
                   "ISTAT=%02x SSTAT0=%02x DSTAT=%02x SIEN=%02x\n",
                    a4091_save.intcount, a4091_save.ireg_istat,
                    a4091_save.ireg_sstat0, a4091_save.ireg_dstat,
                    a4091_save.ireg_sien);
        }
    }
}

/*
 * a4091_cleanup
 * -------------
 * Called at program exit
 */
static void
a4091_cleanup(void)
{
    a4091_state_restore();
}

/*
 * a4091_state_takeover
 * --------------------
 * Sets up a private interrupt handler and captures A4091 state.
 */
static void
a4091_state_takeover(void)
{
    uint64_t tick_start;

    if (a4091_save.cleanup_installed == 0) {
        a4091_save.cleanup_installed = 1;
        atexit(a4091_cleanup);
    }
    if (a4091_save.card_owned == 0) {
        a4091_save.card_owned = 1;

        /*
         * Save procedure:
         * 1. Capture interrupts
         * 2. Capture SCRIPTS processor run/stop state.
         * 3. If running, first suspend the SCRIPTS processor by putting
         *    it in single step mode. Wait for it to stop.
         * 4. Save all read/write registers.
         */
        a4091_add_local_irq_handler();
        a4091_disable_driver_irq_handler(0);

        a4091_save.reg_istat = get_ncrreg8(REG_ISTAT);

#if 0
        /* Stop SCRIPTS processor */
        if (!is_running_in_uae()) {
            /* Below line causes segfault or hang of FS-UAE */
            // XXX: How can we tell if the SCRIPTS processor is running?
            printf("Stopping SIOP\n");
            set_ncrreg8(REG_ISTAT, a4091_save.reg_istat | REG_ISTAT_ABRT);
            (void) get_ncrreg8(REG_ISTAT);

            tick_start = read_system_ticks();
            while ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_ABRT) == 0) {
                if (access_timeout("DSTAT_ABRT timeout", 2, tick_start))
                    break;
            }
        }
#endif

#if 1
        /* Soft reset SCRIPTS processor (SIOP) */
        if (runtime_flags & FLAG_DEBUG)
            printf("Soft resetting SIOP\n");
        set_ncrreg8(REG_ISTAT, a4091_save.reg_istat | REG_ISTAT_RST);
        (void) get_ncrreg8(REG_ISTAT);
        set_ncrreg8(REG_ISTAT, a4091_save.reg_istat);
#endif
#if 0
        a4091_save.reg_dcntl = get_ncrreg8(REG_DCNTL);
        set_ncrreg8(REG_DCNTL, a4091_save.reg_dcntl | REG_DCNTL_SSM);
        (void) get_ncrreg8(REG_DCNTL);

        tick_start = read_system_ticks();
        while ((a4091_save.ireg_istat & REG_ISTAT_RST) == 0) {
            if (access_timeout("ISTAT_RST timeout", 2, tick_start))
                break;
        }
#endif

        /* Reset NCR 53C710 */
        set_ncrreg8(REG_ISTAT, a4091_save.reg_istat | REG_ISTAT_RST);

        tick_start = read_system_ticks();
        while ((get_ncrreg8(REG_ISTAT) & REG_ISTAT_RST) == 0)
            if (access_timeout("ISTAT_RST timeout", 2, tick_start))
                break;

        set_ncrreg8(REG_ISTAT, a4091_save.reg_istat & ~REG_ISTAT_RST);

#if 0
        a4091_save.reg_00     = get_ncrreg32(0x00); // SIEN SDID SCNTL1 SCNTL0
        a4091_save.reg_04     = get_ncrreg32(0x04); // SOCL SODL SXFER SCID
        a4091_save.reg_08     = get_ncrreg32(0x08); // SBCL SBDL SIDL SFBR
        a4091_save.reg_10     = get_ncrreg32(0x10); // DSA
        a4091_save.reg_1c     = get_ncrreg32(0x1c); // TEMP
        a4091_save.reg_24     = get_ncrreg32(0x24); // DCMD DBC
        a4091_save.reg_28     = get_ncrreg32(0x28); // DNAD
        a4091_save.reg_2c     = get_ncrreg32(0x2c); // DSP
        a4091_save.reg_30     = get_ncrreg32(0x30); // DSPS
        a4091_save.reg_34     = get_ncrreg32(0x34); // SCRATCH
        a4091_save.reg_38     = get_ncrreg32(0x38); // DNCTL DWT DIEN DMODE
        a4091_save.reg_3c     = get_ncrreg32(0x3c); // ADDER
#endif
    }
}


/*
 * To perform a read from memory, make the destination address be
 * within the 53C710's address space. To perform a write to memory,
 * make the source address be within the 53C710. If both addresses
 * are in system memory, then the 53C710 functions as a high-speed
 * DMA copy peripheral.
 *
 * 0x98080000 = Transfer Control, Opcode=011 (Interrupt)
 * 0xc8080000 = Memory-to-memory copy
 */
__attribute__((aligned(32)))
uint32_t dma_mem_move_script[] = {
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination adddress (TEMP)
    0x98080000, 0x00000000,  // Transfer Control Opcode=011 (Interrupt and stop)
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000               // Nop
};

uint32_t dma_mem_move_script_quad[] = {
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination adddress (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination adddress (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination adddress (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination adddress (TEMP)
    0x98080000, 0x00000000,  // Transfer Control Opcode=011 (Interrupt and stop)
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000, 0x00000000,  // Nop
    0x00000000               // Nop
};

static void
dma_init_siop(void)
{
    uint8_t istat;

    if (runtime_flags & FLAG_MORE_DEBUG)
        printf("Initializing SIOP\n");

    a4091_abort();
    a4091_reset();

    /* SCLK=37.51-50.0 MHz, 53C710 */
    set_ncrreg8(REG_DCNTL, REG_DCNTL_CFD2 | REG_DCNTL_COM);

    const int burst_mode = 8;
    switch (burst_mode) {
        default:
        case 1:
            /* 1-transfer burst, FC = 101 -- works on A3000 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE0 | REG_DMODE_FC2);
            break;
        case 2:
            /* 2-transfer burst, FC = 101 -- seems to work on A3000 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE1 | REG_DMODE_FC2);
            break;
        case 4:
            /* 4-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE2 | REG_DMODE_FC2);
            break;
        case 8:
            /* 8-transfer burst, FC = 101 -- hangs on A3000 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE3 | REG_DMODE_FC2);
            break;
    }

    /* Disable cache line bursts */
    set_ncrreg8(REG_CTEST7, get_ncrreg8(REG_CTEST7) | REG_CTEST4_CDIS);

    /* Clear pending interrupts */
    while (((istat = get_ncrreg8(REG_ISTAT)) & 0x03) != 0) {
        if (istat & REG_ISTAT_SIP)
            (void) get_ncrreg8(REG_SSTAT0);

        if (istat & REG_ISTAT_DIP)
            (void) get_ncrreg8(REG_DSTAT);

        if (istat & (REG_ISTAT_DIP | REG_ISTAT_SIP))
            Delay(1);
    }
}

static int
execute_script(uint32_t *script)
{
    int rc = 0;
    int count = 0;
    uint64_t tick_start;

    a4091_save.ireg_istat = 0;

    set_ncrreg32(REG_DSP, (uint32_t) script);

    tick_start = read_system_ticks();
    while (1) {
        if ((count & 7) == 0) {
            uint8_t istat = get_ncrreg8(REG_ISTAT);
            if (istat & (REG_ISTAT_ABRT | REG_ISTAT_DIP)) {
                (void) get_ncrreg8(REG_DSTAT);
                if (runtime_flags & FLAG_DEBUG)
                    printf("Got DMA polled completion\n");
                break;
            }
        }
        if (a4091_save.ireg_istat & (REG_ISTAT_ABRT | REG_ISTAT_DIP)) {
            if (runtime_flags & FLAG_DEBUG)
                printf("Got DMA completion interrupt\n");
            break;
        }

        if (((count & 31) == 0) &&
            access_timeout("SIOP timeout", 30, tick_start)) {
            printf("ISTAT=%02x %02x DSTAT=%02x SSTAT0=%02x SSTAT1=%02x "
                   "SSTAT2=%02x\n",
                   a4091_save.ireg_istat, get_ncrreg8(REG_ISTAT),
                   get_ncrreg8(REG_DSTAT), get_ncrreg8(REG_SSTAT0),
                   get_ncrreg8(REG_SSTAT1), get_ncrreg8(REG_SSTAT2));
            rc = 1;
            goto fail;
        }
        count++;
    }

fail:
    return (rc);
}

static void
print_bits(bitdesc_t *bits, uint value)
{
    uint bit;
    for (bit = 0; value != 0; value >>= 1, bit++) {
        if (value & 1) {
            printf(" %s", bits[bit]);
        }
    }
}

static int
decode_registers(void)
{
    const char *fmt;
    int         reg;
    uint32_t    value;

    printf("  Reg    Value  Name     Description\n");

    for (reg = 0; reg < ARRAY_SIZE(ncr_regdefs); reg++) {
        if (ncr_regdefs[reg].show == 0)
            continue;
        printf("   %02x ", ncr_regdefs[reg].reg_loc);

        if (ncr_regdefs[reg].reg_size == 1) {
            value = get_ncrreg8(ncr_regdefs[reg].reg_loc);
        } else {
            value = get_ncrreg32(ncr_regdefs[reg].reg_loc & ~3);
            value &= (0xffffffff >> ((ncr_regdefs[reg].reg_loc & 3) * 8));
        }
        switch (ncr_regdefs[reg].reg_size) {
            case 1:
                fmt = "      %0*x";
                break;
            case 2:
                fmt = "    %0*x";
                break;
            case 3:
                fmt = "  %0*x";
                break;
            default:
                fmt = "%0*x";
                break;
        }
        printf(fmt, ncr_regdefs[reg].reg_size * 2, value);
        printf("  %-8s %s",
               ncr_regdefs[reg].reg_name, ncr_regdefs[reg].reg_desc);
        if (ncr_regdefs[reg].reg_bits != NULL) {
            print_bits(ncr_regdefs[reg].reg_bits, value);
        }
        printf("\n");
    }
    return (0);
}


#if 0
static int
dma_mem_to_fifo(uint32_t src, uint32_t len)
{
    uint8_t ctest5 = get_ncrreg8(REG_CTEST5);
    uint8_t ctest8 = get_ncrreg8(REG_CTEST8);

    /* Clear DMA and SCSI FIFOs */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_CLF);
    (void) get_ncrreg8(REG_CTEST8);
    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_CLF);

    /* Set DMA direction host->SCSI and disable SCSI request/ack of DMA */
    set_ncrreg8(REG_CTEST5, ctest5 &
                ~(REG_CTEST5_DACK | REG_CTEST5_DREQ | REG_CTEST5_DDIR));

    /* Assign source or destination address */
    set_ncrreg32(REG_DNAD, src);

    /*
     * Set DMA command and byte count
     * DMA command is high byte
     *   01      = Read/Write Instruction Register
     *     110   = Move to SFBR
     *        00 = Immediate data to destination register
     */
    set_ncrreg32(REG_DCMD, 0x40000000 | len);
    // DCMD and DBC (command & byte count)

#if 1
    printf("DBC1=%x", get_ncrreg32(REG_DCMD));
#endif
    /* Force DMA */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_FLF);
    Delay(1);

    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_FLF);
#if 1
    (void) decode_registers();
    printf(" DBC2=%x\n", get_ncrreg32(REG_DCMD));
#endif

    return (0);
}
#endif

#if 0
/*
 * dma_mem_to_scratch
 * ------------------
 * This function does not work. It was an attempt to implement a
 * CPU-configured test where the only fetch from Amiga memory would be
 * due to the DMA. This would have eliminated the possibility that an
 * errant fetch of the next SIOP instruction could cause a bad DMA write to
 * Amiga memory. This could occur if there are floating or bridged address
 * lines between the 53C710 and the 4091 bus tranceivers. Unfortunately,
 * I could not figure out a way to get the 53C710 to execute the command
 * written to the REG_DCMD register.
 */
static int
dma_mem_to_scratch(uint32_t src)
{
    uint8_t ctest5 = get_ncrreg8(REG_CTEST5);
    uint8_t ctest8 = get_ncrreg8(REG_CTEST8);
    uint8_t dcntl  = get_ncrreg8(REG_DCNTL);
    uint8_t dmode  = get_ncrreg8(REG_DMODE);

    set_ncrreg8(REG_DMODE, dmode | REG_DMODE_MAN);
    set_ncrreg8(REG_DCNTL, dcntl | REG_DCNTL_SSM);

    /* Clear DMA and SCSI FIFOs */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_CLF);
    (void) get_ncrreg8(REG_CTEST8);
    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_CLF);

    /* Set DMA direction host->SCSI and disable SCSI request/ack of DMA */
    set_ncrreg8(REG_CTEST5, ctest5 &
                ~(REG_CTEST5_DACK | REG_CTEST5_DREQ | REG_CTEST5_DDIR));

    printf("src=%x [%08x]\n", src, *(uint32_t *) src);

    /* Assign source address */
    set_ncrreg32(REG_DSPS, src);

    /* Assign destination address */
    set_ncrreg32(REG_TEMP, a4091_base + A4091_OFFSET_REGISTERS + REG_SCRATCH);

    /*
     * Set DMA command and byte count
     * DMA command is high byte
     *   11               = Memory Move
     *     000000         = Reserved
     *           ... 0100 = Length (4 bytes)
     */
    set_ncrreg32(REG_DCMD, 0xc0000000 | 0x04);

    printf("DBC1=%x", get_ncrreg32(REG_DCMD));

#if 0
    set_ncrreg8(REG_DCNTL, dcntl | REG_DCNTL_SSM | REG_DCNTL_STD);
#endif

    /* Force DMA */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_FLF);
    Delay(1);

    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_FLF);
    set_ncrreg8(REG_DCNTL, dcntl);
    set_ncrreg8(REG_DMODE, dmode);
#if 0
    (void) decode_registers();
    printf(" DBC2=%x\n", get_ncrreg32(REG_DCMD));
#endif

    return (0);
}
#endif

/*
 * dma_mem_to_mem
 * --------------
 * Perform a memory-to-memory copy using the 53C710 DMA engine.
 */
static int
dma_mem_to_mem(uint32_t src, uint32_t dst, uint32_t len)
{
    uint32_t *script = dma_mem_move_script;

    script[0] = 0xc0000000 | len;
    script[1] = src;
    script[2] = dst;
    CACHE_LINE_WRITE(script, 0x10);

    if (runtime_flags & FLAG_DEBUG)
        printf("DMA from %08x to %08x len %08x\n", src, dst, len);

    a4091_state_takeover();
    a4091_save.ireg_istat = 0;

#if 0
    /* Hold destination address constant */
    set_ncrreg8(REG_DMODE, get_ncrreg8(REG_DSTAT) | REG_DMODE_FAM);
#endif

    return (execute_script(script));
}

/*
 * dma_mem_to_scratch
 * -------------------
 * Copy 4 bytes of memory to the 53C710 SCRATCH register.
 */
static int
dma_mem_to_scratch(uint32_t src)
{
    uint32_t dst = a4091_base + A4091_OFFSET_REGISTERS + REG_SCRATCH;
    return (dma_mem_to_mem(src, dst, 4));
}

/*
 * dma_mem_to_mem_quad
 * -------------------
 * Performs four memory-to-memory copies of the same data for benchmarking
 * purposes.
 */
static int
dma_mem_to_mem_quad(volatile APTR src, volatile APTR dst, uint32_t len,
                    int update_script)
{
    uint32_t *script = dma_mem_move_script_quad;

    if (update_script) {
        ULONG xlen = sizeof (dma_mem_move_script_quad);
        script[0] = 0xc0000000 | len;
        script[1] = (uint32_t) src;
        script[2] = (uint32_t) dst;
        script[3] = 0xc0000000 | len;
        script[4] = (uint32_t) src;
        script[5] = (uint32_t) dst;
        script[6] = 0xc0000000 | len;
        script[7] = (uint32_t) src;
        script[8] = (uint32_t) dst;
        script[9] = 0xc0000000 | len;
        script[10] = (uint32_t) src;
        script[11] = (uint32_t) dst;

        CachePreDMA((APTR) script, &xlen, DMA_ReadFromRAM);
        CachePostDMA((APTR) script, &xlen, DMA_ReadFromRAM);
//      CACHE_LINE_WRITE(script, 0x30);

        a4091_state_takeover();
    }

    if (runtime_flags & FLAG_DEBUG)
        printf("DMA from %08x to %08x len %08x\n",
               (uint32_t) src, (uint32_t) dst, len);

    a4091_state_takeover();
    a4091_save.ireg_istat = 0;

#if 0
    /* Hold destination address constant */
    set_ncrreg8(REG_DMODE, get_ncrreg8(REG_DSTAT) | REG_DMODE_FAM);
#endif

    return (execute_script(script));
}

static void
show_dip(uint8_t switches, int bit)
{
    printf("  SW %d %s  ", bit + 1, (switches & BIT(7)) ? "Off" : "On");
}

static int
decode_switches(void)
{
    uint8_t switches = *ADDR8(a4091_base + A4091_OFFSET_SWITCHES);
    printf("A4091 Rear-access DIP switches\n");
    show_dip(switches, 7);
    printf("SCSI LUNs %s\n", (switches & BIT(7)) ? "Enabled" : "Disabled");
    show_dip(switches, 6);
    if (switches & BIT(6))
        printf("Internal Termination On\n");
    else
        printf("External Termination Only\n");
    show_dip(switches, 5);
    printf("%s SCSI Mode\n",
           (switches & BIT(5)) ? "Synchronous" : "Asynchronous");
    show_dip(switches, 4);
    printf("%s Spinup\n", (switches & BIT(4)) ? "Short" : "Long");
    show_dip(switches, 3);
    printf("SCSI%s Bus Mode\n",
           (switches & BIT(3)) ? "-2 Fast" : "-1 Standard");
    show_dip(switches, 2);
    printf("ADR2=%d\n", !!(switches & 4));
    show_dip(switches, 1);
    printf("ADR1=%d\n", !!(switches & 2));
    show_dip(switches, 0);
    printf("ADR0=%d  Controller Host ID=%x\n", switches & 1, switches & 7);

    return (0);
}

/*
 * Test overview
 * -------------
 * Device Access
 * Register Access
 * SCSI FIFO
 * DMA FIFO
 * DMA operations
 * DMA copy
 * DMA copy benchmark
 * Interrupts
 * Loopback
 * SCRIPTS Processor
 *
 * =========================================================================
 *
 * Device Access
 * -------------
 * Check for bus timeout
 * Autoconfig area at address
 * Registers found at address
 *
 *
 * Register Access
 * ---------------
 * Registers have sane values
 * Reset chip via registers
 * Verify default (cleared) state of status registers
 * Walking bits test of two writable registers (SCRATCH and TEMP)
 *
 *
 * DMA FIFO Test
 * -------------
 * Write data into DMA FIFO and verify it is retrieved in the same order.
 *
 * Test the basic ability to write data into the DMA FIFO and retrieve
 * it in the same order as written. The DMA FIFO is checked for an empty
 * condition following a software reset, then the FBL2 bit is set and
 * verified. The FIFO is then filled with 16 bytes of data in the four
 * byte lanes verifying the byte lane full or empty with each write. Next
 * the FIFO is read verifying the data and the byte lane full or empty
 * with each read. If no errors are detected then the NCR device is reset,
 * otherwise the device is left in the test state.
 *
 *
 * SCSI FIFO test
 * --------------
 * Write data into SCSI FIFO and verify it is retrieved in the same order.
 *
 * Tests the basic ability to write data into the SCSI FIFO and retrieve
 * it in the same order as written. The SCSI FIFO is checked for an
 * empty condition following a software reset, then the SFWR bit is set
 * and verified. The FIFO is then filled with 8 bytes of data verifying
 * the byte count with each write.  Next the SFWR bit is cleared and the
 * FIFO read verifying the byte count with each read. If no errors are
 * detected then the NCR device is reset, otherwise the device is left
 * in the test state.
 *
 *
 * DMA operations
 * --------------
 * Test DMA from/to host memory.
 *
 *
 * Interrupt test
 * --------------
 * Verifies that level 0 interrupts will not generate an interrupt,
 * but will set the appropriate status. The test then verifies that all
 * interrupts (1-7) can be generated and received and that the appropriate
 * status is set.
 *
 *
 * Loopback test
 * -------------
 * The 53C710 Loopback Mode in effect, lets the chip talk to itself. When
 * the Loopback Enable (SLBE) bit is set in the CTEST4 register, the
 * 53C710 allows control of all SCSI signals. The test checks the Input
 * and Output Data Latches and performs a selection, with the 53C710
 * executing initiator instructions and the host CPU implementing the
 * target role by asserting and polling the appropriate SCSI signals.
 * If no errors are detected then the NCR device is reset, otherwise the
 * device is left in the test state.
 *
 *
 * SCSI SCRIPTS processor possible tests
 * -------------------------------------
 * SCSI Interrupt Enable
 * DMA Interrupt Enable
 * SCSI Status Zero
 * DMA Status
 * Interrupt Status
 * SCSI First Byte Received
 *
 * Set SCSI outputs in high impedance state, disable interrupts using
 * the "MIEN", and set NCR device for Single Step Mode.
 *
 * The address of a simple "INTERRUPT instruction" SCRIPT is loaded
 * into the DMA SCRIPTS Pointer register. The SCRIPTS processor is
 * started by hitting the "STD" bit in the DMA Control Register.
 *
 * Single Step is checked by verifying that ONLY the first instruction
 * executed and that the correct status bits are set. Single Step Mode
 * is then turned off and the SCRIPTS processor started again. The
 * "INTERRUPT instruction" should then be executed and a check for the
 * correct status bits set is made.
 *
 * The address of the "JUMP instruction" SCRIPT is loaded into the DMA
 * SCRIPTS Pointer register, and the SCRIPTS processor is automatically
 * started.  JUMP "if TRUE" (Compare = True, Compare = False) conditions
 * are checked, then JUMP "if FALSE" (Compare = True, Compare = False)
 * conditions are checked.
 */

static int
check_ncrreg_bits(int mode, uint reg, const char *regname, uint8_t rbits)
{
    uint8_t regval = get_ncrreg8(reg);

    if (regval & rbits) {
        const char *modestr = (mode == 0) ? "reserved" : "unexpected";
        printf("%s reg %02x [value %02x] has %s bits set: %02x\n",
               regname, reg, regval, modestr, regval & rbits);
        return (1);
    }
    return (0);
}

static void
show_test_state(const char * const name, int state)
{
    if (state == 0) {
        printf("PASS\n");
        return;
    }
    printf("  %-15s ", name);
    if (state == -1) {
        fflush(stdout);
        return;
    }
    printf("FAIL\n");
}

/*
 * test_device_access
 * ------------------
 * A4091 device is verified for basic access.
 *
 * 1. Check for bus timeout to ROM area
 * 2. Check for bus timeout to 53C710 registers
 * 3. Verify autoconfig area header contents
 */
static int
test_device_access(void)
{
    static const uint8_t zorro_expected_regs[] = {
        0x6f, 0x54, 0x30, 0x00, 0x02, 0x02
    };
#define NUM_ZORRO_EXPECTED_REGS ARRAY_SIZE(zorro_expected_regs)
    uint8_t  saw_correct[NUM_ZORRO_EXPECTED_REGS];
    uint8_t  saw_incorrect[NUM_ZORRO_EXPECTED_REGS];
    int      i;
    int      rc = 0;
    int      pass;
    uint64_t tick_start;

    show_test_state("Device access:", -1);

    memset(saw_correct, 0, sizeof (saw_correct));
    memset(saw_incorrect, 0, sizeof (saw_incorrect));

    /* Measure access speed against possible bus timeout */
    tick_start = read_system_ticks();
    (void) *ADDR32(a4091_base + A4091_OFFSET_ROM);
    if (access_timeout("ROM access timeout", 2, tick_start)) {
        /* Try again */
        (void) *ADDR32(a4091_base + A4091_OFFSET_ROM);
        if (access_timeout("ROM access timeout", 2, tick_start)) {
            rc = 1;
            goto fail;
        }
    }

    (void) *ADDR32(a4091_base + A4091_OFFSET_REGISTERS);
    if (access_timeout("\n53C710 access timeout", 2, tick_start)) {
        rc = 1;
        goto fail;
    }

    /* Verify autoconfig area header contents */
    for (pass = 0; pass < 100; pass++) {
        tick_start = read_system_ticks();
        for (i = 0; i < NUM_ZORRO_EXPECTED_REGS; i++) {
            uint8_t regval = get_creg(i * 4);
            if (access_timeout("\n53C710 loop access timeout", 4, tick_start)) {
                rc = 1;
                goto fail;
            }
            if (regval == zorro_expected_regs[i]) {
                saw_correct[i] = 1;
            } else {
                if (saw_incorrect[i] == 0) {
                    saw_incorrect[i] = 1;
                    if (rc == 0)
                        printf("\n");
                    printf("    Reg %02x  %02x != expected %02x (diff %02x)\n",
                           i * 4, regval, zorro_expected_regs[i],
                           regval ^ zorro_expected_regs[i]);
                    rc++;
                }
            }
        }
    }

fail:
    show_test_state("Device access:", rc);
    return (rc);
}

static int
is_running_in_uae(void)
{
    struct IntVector *iv    = &SysBase->IntVects[A4091_IRQ];
    struct List      *slist = iv->iv_Data;
    struct Interrupt *s;

    if (EMPTY(slist))
        return (0);

    for (s = FIRST(slist); NEXTINT(s); s = NEXTINT(s)) {
        const char *name = GetNodeName((struct Node *) s);
        if (strncmp(name, "UAE", 3) == 0)
            return (1);
    }
    return (0);
}

static bitdesc_t data_pins[] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "D8", "D9", "D10", "D11", "D12", "D13", "D14", "D15",
    "D16", "D17", "D18", "D19", "D20", "D21", "D22", "D23",
    "D24", "D25", "D26", "D27", "D28", "D29", "D30", "D31",
};

/*
 * test_register_access
 * --------------------
 * 1. Verify 53C710 reserved register bits are as expected
 * 2. Reset chip via registers
 * 3. Verify default (cleared) state of status registers
 * 4. Walking bits test of writable registers
 */
static int
test_register_access(void)
{
    int pass;
    int rc = 0;
    uint32_t stuck_high;
    uint32_t stuck_low;
    uint32_t pins_diff;
    uint32_t patt = 0xf0e7c3a5;
    uint32_t next;
    uint     rot;

    show_test_state("Register test:", -1);

    /* Verify reserved bits are as expected */
    for (pass = 0; pass < 100; pass++) {
        rc += check_ncrreg_bits(0, REG_SCNTL1, "SCNTL1", BIT(1) | BIT(0));
        rc += check_ncrreg_bits(0, REG_DSTAT, "DSTAT", BIT(6));
        rc += check_ncrreg_bits(0, REG_CTEST0, "CTEST0", BIT(7) | BIT(1));
        rc += check_ncrreg_bits(0, REG_CTEST0, "CTEST2", BIT(7));
        rc += check_ncrreg_bits(0, REG_ISTAT, "ISTAT", BIT(4) | BIT(2));
        rc += check_ncrreg_bits(0, REG_DIEN, "DIEN", BIT(7) | BIT(6));

        if (rc != 0)
            break;
    }

    a4091_reset();

    /* Verify status registers have been cleared */
    rc += check_ncrreg_bits(1, REG_ISTAT, "ISTAT", 0xff);
    rc += check_ncrreg_bits(1, REG_DSTAT, "DSTAT", 0x7f);

    /* Walking bits test of writable registers (TEMP and SCRATCH) */
    stuck_high = 0xffffffff;
    stuck_low  = 0xffffffff;
    pins_diff  = 0x00000000;
    for (rot = 0; rot < 256; rot++, patt = next) {
        uint32_t got_scratch;
        uint32_t got_temp;
        uint32_t diff_s;
        uint32_t diff_t;
        next = (patt << 1) | (patt >> 31);
        set_ncrreg32(REG_SCRATCH, patt);
        set_ncrreg32(REG_TEMP, next);
#if 0
        flush_ncrreg32(REG_SCRATCH);
        flush_ncrreg32(REG_TEMP);
#endif
        got_scratch = get_ncrreg32(REG_SCRATCH);
        got_temp    = get_ncrreg32(REG_TEMP);
        stuck_high &= (got_scratch & got_temp);
        stuck_low  &= ~(got_scratch | got_temp);
        diff_s = got_scratch ^ patt;
        diff_t = got_temp    ^ next;
        if (diff_s != 0) {
            pins_diff |= diff_s;
            if (rc++ == 0)
                printf("\n");
            if (rc < 8) {
                printf("Reg SCRATCH %08x != %08x (diff %08x",
                       got_scratch, patt, diff_s);
                print_bits(data_pins, diff_s);
                printf(")\n");
            }
        }
        if (diff_t != 0) {
            pins_diff |= diff_t;
            if (rc++ == 0)
                printf("\n");
            if (rc < 8) {
                printf("Reg TEMP    %08x != %08x (diff %08x",
                       got_temp, next, diff_t);
                print_bits(data_pins, diff_t);
                printf(")\n");
            }
        }
    }
    pins_diff &= ~(stuck_high | stuck_low);
    if (stuck_high != 0) {
        printf("Stuck high: %08x", stuck_high);
        print_bits(data_pins, stuck_high);
        printf(" (check for short to VCC)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck low: %08x", stuck_low);
        print_bits(data_pins, stuck_low);
        printf(" (check for short to GND)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %02x", pins_diff);
        print_bits(data_pins, pins_diff);
        printf("\n");
    }

    show_test_state("Register test:", rc);
    return (rc);
}


/*
 * rand32
 * ------
 * Very simple pseudo-random number generator
 */
static uint32_t rand_seed = 0;
static uint32_t
rand32(void)
{
    rand_seed = (rand_seed * 25173) + 13849;
    return (rand_seed);
}

/*
 * srand32
 * -------
 * Very simple random number seed
 */
static void
srand32(uint32_t seed)
{
    rand_seed = seed;
}


/*
 * test_dma_fifo
 * -------------
 * This test writes data into DMA FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Reset the 53C710 and verify the DMA FIFO is empty.
 * 2. Set FBL bits in CTEST4 while filling each byte lane with 16 bytes,
 *    verifying each FIFO is full (64 bytes total stored).
 * 3. Unload the DMA FIFO, verifying all stuffed data values and that
 *    FIFO status is reported as expected.
 */
static int
test_dma_fifo(void)
{
    int     rc = 0;
    int     lane;
    uint8_t ctest1;
    uint8_t ctest4;
    uint8_t ctest7;
    int     cbyte;

    /* The DMA FIFO test fails in FS-UAE due to incomplete emulation */
    if (is_running_in_uae())
        return (0);

    show_test_state("DMA FIFO test:", -1);

    a4091_reset();

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("DMA FIFO not empty before test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    /* Verify FIFO is empty */
    if ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) == 0) {
        if (rc++ == 0)
            printf("\n");
        printf("DMA FIFO not empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest4 = get_ncrreg8(REG_CTEST4);
    ctest7 = get_ncrreg8(REG_CTEST7) & ~BIT(3);

    /* Push bytes to all byte lanes of DMA FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        /* Select byte lane */
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Push bytes to byte lane of DMA FIFO, including parity */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = rand32() >> 8;
            uint8_t  pvalue = ctest7 | ((rvalue >> 5) & BIT(3));
            // XXX: Verify FIFO is not full
            set_ncrreg8(REG_CTEST7, pvalue);
            set_ncrreg8(REG_CTEST6, rvalue);
            if (runtime_flags & FLAG_DEBUG)
                printf(" %02x", rvalue & 0x1ff);
        }
    }

    /* Verify FIFO is not empty */
    if (get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) {
        if (rc++ == 0)
            printf("\n");
        printf("DMA FIFO is empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0x0f) {
        printf("DMA FIFO not full: CTEST1 should be 0x0f, but is 0x%02x\n",
               ctest1);
        rc = 0xff;
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            goto fail;
    }

    /* Pop bytes from byte lanes of DMA FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        int count = 0;
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Pop bytes from byte lane of DMA FIFO, attaching parity as bit 8 */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = (rand32() >> 8) & (BIT(9) - 1);
            uint16_t value  = get_ncrreg8(REG_CTEST6);
            uint16_t pvalue = (get_ncrreg8(REG_CTEST2) & BIT(3)) << 5;
            value |= pvalue;
            if (value != rvalue) {
                if (((rc & BIT(lane)) == 0) || (count++ < 2)) {
                    if (rc == 0)
                        printf("\n");
                    printf("Lane %d byte %d FIFO got %03x, expected %03x\n",
                           lane, cbyte, value, rvalue);
                } else if (count == 3) {
                    printf("...\n");
                }
                rc |= BIT(lane);
            }
        }
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("\nDMA FIFO not empty after test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        rc = 0xff;
    }

fail:
    /* Restore normal operation */
    set_ncrreg8(REG_CTEST4, ctest4 & ~7);

    show_test_state("DMA FIFO test:", rc);
    return (rc);
}

/*
 * test_scsi_fifo
 * --------------
 * This test writes data into SCSI FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Reset the 53C710 and verify the DMA FIFO is empty.
 * 2. Set SCSI FIFO Write Enable bit in CTEST4.
 * 3. The data is loaded into the SCSI FIFO by writing to the SODL register.
 * 4. FIFO data parity can be written in one of two ways:
 *    A. Parity can flow into the SIOP on the parity signals if the Enable
 *       Parity Generation bit in the SCNTLO register equals O. The PU
 *       drives the parity signal for the corresponding 8-bit data signals.
 *    B. If the Parity Generation bit is equal to 1, then the SIOP forces
 *       the parity bit to even or odd parity. Set the Assert Even SCSI Parity
 *       bit in the SCNTL1 register to 0 to load the SCSI FIFO with odd parity.
 *       If this bit is equal to 1, then the SCSI FIFO will be loaded with even
 *       parity.
 * 5. Read CTEST3 to pull data from the SCSI FIFO. CTEST2 Bit 4 (SCSI FIFO
 *    parity) provides FIFO parity after reading CTEST3.
 * 6. Unload the SCSI FIFO, verifying all stuffed data values and that
 *    FIFO status is reported as expected.
 */
static int
test_scsi_fifo(void)
{
    int     rc = 0;
    int     lane;
    uint8_t ctest1;
    uint8_t ctest4;
    uint8_t ctest7;
    int     cbyte;

#if 0
    /* The SCSI FIFO test fails in FS-UAE due to incomplete emulation */
    if (is_running_in_uae())
        return (0);
#endif

    show_test_state("SCSI FIFO test:", -1);

    // XXX: The below code has not been changed yet from the DMA FIFO test
    a4091_reset();

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("SCSI FIFO not empty before test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    /* Verify FIFO is empty */
    if ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) == 0) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI FIFO not empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest4 = get_ncrreg8(REG_CTEST4);
    ctest7 = get_ncrreg8(REG_CTEST7) & ~BIT(3);

    /* Push bytes to all byte lanes of SCSI FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        /* Select byte lane */
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Push bytes to byte lane of SCSI FIFO, including parity */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = rand32() >> 8;
            uint8_t  pvalue = ctest7 | ((rvalue >> 5) & BIT(3));
            // XXX: Verify FIFO is not full
            set_ncrreg8(REG_CTEST7, pvalue);
            set_ncrreg8(REG_CTEST6, rvalue);
            if (runtime_flags & FLAG_DEBUG)
                printf(" %02x", rvalue);
        }
    }

    /* Verify FIFO is not empty */
    if (get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI FIFO is empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0x0f) {
        printf("SCSI FIFO not full: CTEST1 should be 0x0f, but is 0x%02x\n",
               ctest1);
        rc = 0xff;
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            goto fail;
    }

    /* Pop bytes from byte lanes of SCSI FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        int count = 0;
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Pop bytes from byte lane of SCSI FIFO, attaching parity as bit 8 */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = (rand32() >> 8) & (BIT(9) - 1);
            uint16_t value  = get_ncrreg8(REG_CTEST6);
            uint16_t pvalue = (get_ncrreg8(REG_CTEST2) & BIT(3)) << 5;
            value |= pvalue;
            if (value != rvalue) {
                if (((rc & BIT(lane)) == 0) || (count++ < 2)) {
                    if (rc == 0)
                        printf("\n");
                    printf("Lane %d byte %d FIFO got %02x, expected %02x\n",
                           lane, cbyte, value, rvalue);
                } else if (count == 3) {
                    printf("...\n");
                }
                rc |= BIT(lane);
            }
        }
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("\nSCSI FIFO not empty after test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        rc = 0xff;
    }

fail:
    /* Restore normal operation */
    set_ncrreg8(REG_CTEST4, ctest4 & ~7);

    show_test_state("SCSI FIFO test:", rc);
    return (rc);
}

/*
 * test_loopback
 * -------------
 * This test writes data into DMA FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Put the 53C710 in loopback mode (this lets the host directly modify
 *    SCSI bus dignals).
 * 2. Trigger a selection, with the 53C710 acting as SCSI initiator
 * 3. The host will act as a fake target device.
 * 4. Verify correct selection sequence takes place.
 * 5. Reset the 53C710.
 *
 * From the 53C700 Data manual:
 * How to Test the SIOP in the Loopback Mode
 * -----------------------------------------
 * SIOP loopback mode allows testing of both initiator and target operations.
 * When the Loopback Enable bit is 1 in the CTEST4 register, the SIOP allows
 * control of all SCSI signals, whether the SIOP is operating in initiator
 * or target mode. Perform the following steps to implement loopback mode:
 : 1. Set the Loopback Enable bit in the CTEST4 register to 1.
 * 2. Set-up the desired arbitration mode as defined in the SCNTLO register.
 * 3. Set the Start Sequence bit in the SCNTLO register to 1.
 * 4. Poll the SBCL register to determine when SEU is active and BSYI is
 *    inactive.
 * 5. Poll the SBDL register to determine which SCSI ID bits are being driven.
 * 6. In response to selection, set the BSYI bit (bit 5) in the SOCL register
 *    to 1.
 * 7. Poll the SEU bit in the SBCL register to determine when SEU becomes
 *    inactive.
 * 8. To assert the desired phase, set the MSG/, CID, and I/O bits to the
 *    desired phase in the SOCL register.
 * 9. To assert REQI, keep the phase bits the same and set the REQI bit in
 *    the SOCL register to 1. To accommodate the 400 ns bus settle delay,
 *    set REQI after setting the phase signals,
 * 10. The initiator role can be implemented by single stepping SCSI SCRIPTS
 *     and the SIOP can loopback as a target or vice versa.
 */
#if 0
static int
test_loopback(void)
{
    int     rc = 0;
    int     pass;
    int     bit;
    uint8_t ctest4;

    show_test_state("SIOP loopback test:", -1);

    ctest4 = get_ncrreg8(REG_CTEST4);

    a4091_reset();
    set_ncrreg8(REG_CTEST4, ctest4 | REG_CTEST4_SLBE);




    set_ncrreg8(REG_CTEST4, ctest4);
    a4091_reset();

    show_test_state("SIOP loopback test:", rc);
    return (rc);
}
#endif

static bitdesc_t scsi_data_pins[] = {
    "SCDAT0", "SCDAT1", "SCDAT2", "SCDAT3",
    "SCDAT4", "SCDAT5", "SCDAT6", "SCDAT7",
    "SCDATP",
};
static bitdesc_t scsi_control_pins[] = {
    "SCTRL_IO", "SCTRL_CD", "SCTRL_MSG", "SCTRL_ATN",
    "SCTRL_SEL", "SCTRL_BSY", "SCTRL_ACK", "SCTRL_REQ",
};

static uint
calc_parity(uint8_t data)
{
    data ^= (data >> 4);
    data ^= (data >> 2);
    data ^= (data >> 1);
    return (!(data & 1));
}

/*
 * test_scsi_pins
 * --------------
 * This test uses override bits in the CTEST registers to allow the CPU
 * to set and clear SCSI data and control pin signals. With that access,
 * pins are checked to verify they can be set and cleared, and that those
 * operations do not affect the state of other SCSI pins.
 */
static int
test_scsi_pins(void)
{
    int     rc = 0;
    int     pass;
    int     bit;
    uint8_t ctest4;
    uint8_t dcntl;
    uint8_t scntl0;
    uint8_t scntl1;
    uint8_t sstat1;
    uint8_t sbcl;
    uint8_t sbdl;
    uint    stuck_high;
    uint    stuck_low;
    uint    pins_diff;

    show_test_state("SCSI pin test:", -1);

    ctest4 = get_ncrreg8(REG_CTEST4);
    scntl0 = get_ncrreg8(REG_SCNTL0);
    scntl1 = get_ncrreg8(REG_SCNTL1);
    dcntl  = get_ncrreg8(REG_DCNTL);

    a4091_reset();
    Delay(1);

    /* Check that SCSI termination power is working */
    sbdl = get_ncrreg8(REG_SBDL);
    sbcl = get_ncrreg8(REG_SBCL);
    sbcl |= 0x20; // Not sure why, but STRCL_BSY might still be high
    if ((sbcl == 0xff) && (sbdl == 0xff)) {
        if (rc++ == 0)
            printf("\n");
        printf("All SCSI pins low (check term power D309A and F309A/F309B)\n");
        return (rc);
    }

    /* Check that bus is not stuck in reset */
    sstat1 = get_ncrreg8(REG_SSTAT1);
    if (sstat1 & REG_SSTAT1_RST) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI bus is in reset (check for SCTRL_RST short to GND)\n");
        return (rc);
    }

    /* Test reset */
    set_ncrreg8(REG_SCNTL1, REG_SCNTL1_RST);
    Delay(1);
    sstat1 = get_ncrreg8(REG_SSTAT1);
    if ((sstat1 & REG_SSTAT1_RST) == 0) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI bus cannot be reset (check for SCTRL_RST short to VCC)\n");
    }
    set_ncrreg8(REG_SCNTL1, 0);
    Delay(1);

    /* Set registers to manually drive SCSI data and control pins */
    set_ncrreg8(REG_DCNTL,  dcntl | REG_DCNTL_LLM);
    set_ncrreg8(REG_CTEST4, ctest4 | REG_CTEST4_SLBE);
    set_ncrreg8(REG_SCNTL0, REG_SCNTL0_EPG);
    set_ncrreg8(REG_SCNTL1, REG_SCNTL1_ADB);

    /* Walk a test pattern on SODL and verify that it arrives on SBDL */
    set_ncrreg8(REG_SOCL, 0x00);
    stuck_high = 0x1ff;
    stuck_low  = 0x1ff;
    pins_diff  = 0x000;
    for (pass = 0; pass < 2; pass++) {
        for (bit = -1; bit < 8; bit++) {
            uint    din;
            uint    dout = BIT(bit);
            uint    diff;
            uint8_t parity_exp;
            uint8_t parity_got;

            /*
             * Pass 0 = Walking ones
             * Pass 1 = Walking zeros
             */
            if (pass == 1)
                dout = (uint8_t) ~dout;

            set_ncrreg8(REG_SODL, dout);
            din = get_ncrreg8(REG_SBDL);
            parity_got = get_ncrreg8(REG_SSTAT1) & REG_SSTAT1_PAR;
            parity_exp = calc_parity(dout);
            dout |= (parity_exp << 8);
            din  |= (parity_got << 8);
            stuck_high &= din;
            stuck_low  &= ~din;
            diff = din ^ dout;

            if ((diff & 0xff) != 0)
                diff &= 0xff;  // Ignore parity when other bits differ
            if (diff != 0) {
                pins_diff |= diff;
                if (rc++ == 0)
                    printf("\n");
                if (rc <= 8) {
                    printf("SCSI data %03x != expected %03x (diff %03x",
                           din, dout, diff);
                    print_bits(scsi_data_pins, diff);
                    printf(")\n");
                }
            }
        }
    }
    /* Note: Register state is inverted from SCSI pin state */
    pins_diff &= ~(stuck_high | stuck_low);
    if (stuck_high != 0) {
        printf("Stuck low: %02x", stuck_high);
        print_bits(scsi_data_pins, stuck_high);
        printf(" (check for short to GND)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck high: %02x", stuck_low);
        print_bits(scsi_data_pins, stuck_low);
        printf(" (check for short to VCC)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %02x", pins_diff);
        print_bits(scsi_data_pins, pins_diff);
        printf("\n");
    }

    set_ncrreg8(REG_SODL, 0xff);

    /* Walk a test pattern on SOCL and verify that it arrives on SBCL */
    stuck_high = 0xff;
    stuck_low  = 0xff;
    pins_diff  = 0x00;
    for (pass = 0; pass < 2; pass++) {
        for (bit = -1; bit < 8; bit++) {
            uint8_t din;
            uint8_t dout = BIT(bit);
            uint8_t diff;

            /*
             * Pass 0 = Walking ones
             * Pass 1 = Walking zeros
             */
            if (pass == 1)
                dout = (uint8_t) ~dout;

            /*
             * Eliminate testing certain combinations
             * Never assert bit 3 (SCTRL_SEL)
             */
            if ((dout == 0x80) || (dout == 0x40) || (dout == 0xf7) ||
                (dout & BIT(3)))
                continue;

            set_ncrreg8(REG_SOCL, dout);
            din = get_ncrreg8(REG_SBCL);
            stuck_high &= din;
            stuck_low  &= ~din;
            diff = din ^ dout;
            if (diff != 0) {
                pins_diff |= diff;
                if (rc++ == 0)
                    printf("\n");
                if (rc <= 8) {
                    printf("SCSI control %02x != expected %02x (diff %02x",
                           din, dout, diff);
                    print_bits(scsi_control_pins, diff);
                    printf(")\n");
                }
            }
        }
    }

    /* Note: Register state is inverted from SCSI pin state */
    stuck_low  &= ~(BIT(3) | BIT(6) | BIT(7));
    pins_diff  &= ~(stuck_high | stuck_low);

    if (stuck_high != 0) {
        printf("Stuck low: %02x", stuck_high);
        print_bits(scsi_control_pins, stuck_high);
        printf(" (check for short to GND)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck high: %02x", stuck_low);
        print_bits(scsi_control_pins, stuck_low);
        printf(" (check for short to VCC)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %02x", pins_diff);
        print_bits(scsi_control_pins, pins_diff);
        printf("\n");
    }

    set_ncrreg8(REG_DCNTL,  dcntl);
    set_ncrreg8(REG_SCNTL0, scntl0);
    set_ncrreg8(REG_SCNTL1, scntl1);
    set_ncrreg8(REG_CTEST4, ctest4);
    a4091_reset();

    show_test_state("SCSI pin test:", rc);
    return (rc);
}

/*
 * AllocMem_aligned
 * ----------------
 * Allocate CPU memory with the specified minimum alignment.
 */
static APTR
AllocMem_aligned(uint len, uint alignment)
{
    APTR addr;
    Forbid();
    addr = AllocMem(len + alignment, MEMF_PUBLIC);
    if (addr != NULL) {
        FreeMem(addr, len + alignment);
        addr = (APTR) AllocAbs(len, (APTR) (((uint32_t) addr + alignment - 1) &
                                            ~(alignment - 1)));
    }
    Permit();
    return (addr);
}

/*
 * mem_not_zero
 * ------------
 * Return a non-zero value if the specified memory block is not all zero.`
 */
static int
mem_not_zero(uint32_t paddr, uint len)
{
    uint32_t *addr = (uint32_t *)paddr;
    len >>= 2;
    while (len-- > 0)
        if (*(addr++) != 0)
            return (1);
    return (0);
}

/*
 * test_dma
 * --------
 * This function will repeatedly perform many DMA operations as reads
 * from CPU memory and writes directly to the 53C710's SCRATCH register.
 * Data from each DMA operation is verified by the CPU reading the
 * SCRATCH register. The source address in CPU memory is incremented for
 * each read. This allows a crude memory address test to be implemented,
 * at least for the address bits A10-A0.
 *
 * Could also implement a SCRIPTS function to move from a memory location
 * into the SCRATCH register. I don't like this so much because the
 * SCRIPTS instruction would need to be fetched from RAM. Could mitigate
 * somewhat by configuring single step mode (DCNTL.bit4).
 */
static int
test_dma(void)
{
    int      rc = 0;
    int      rc2 = 0;
    int      pos;
    uint     dma_len = 2048;
    APTR    *tsrc;
    APTR    *src;
    ULONG    buf_handled;
    uint32_t diff;
    uint32_t saddr;
    uint32_t scratch;

    srand32(time(NULL));
    show_test_state("DMA test:", -1);

    tsrc = AllocMem_aligned(dma_len * 3, dma_len);
    if (tsrc == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    src = tsrc + dma_len;

    a4091_reset();

    buf_handled = 4;

    /* First phase is a sequence of transfers to the SCRATCH register */
    for (pos = 0; pos < dma_len; pos += 4) {
        saddr = (uint32_t) src + pos;
        *(uint32_t *) saddr = rand32();
        CachePreDMA((APTR) saddr, &buf_handled, DMA_ReadFromRAM);
        rc = dma_mem_to_scratch(saddr);
        CachePostDMA((APTR) saddr, &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            printf("DMA failed at pos %x\n", pos);
            goto fail_dma;
        }

        scratch = get_ncrreg32(REG_SCRATCH);
        diff = *(uint32_t *)saddr ^ scratch;
        if (diff != 0) {
            /*
             * This test is not aborted on data mismatch errors, so that
             * multiple errors may be captured and reported.
             */
            if (rc2++ < 10) {
                printf("\n  Addr %08x to scratch %08x: %08x != expected %08x "
                       "(diff %08x)\n",
                       saddr, a4091_base + A4091_OFFSET_REGISTERS + REG_SCRATCH,
                       scratch, *(uint32_t *)saddr, diff);
            }
        }
    }
    rc += rc2;

#if 0
    /* XXX: Add more tests in the future */
    for (pass = 0; pass < 1; pass++) {
        for (pos = 0; pos < dma_len; pos += 4) {
            *(uint32_t *) ((uint32_t) src + pos) = rand32();
        }
        buf_handled = dma_len;
        CachePreDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
#if 0
        rc = dma_mem_to_fifo((uint32_t) src, dma_len);
#elif 1
        rc = dma_mem_to_scratch((uint32_t) src);
#else
        rc = dma_mem_to_mem((uint32_t) src, 4);
#endif
        CachePostDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);

        /* Data should now be in DMA FIFOs */
    }
#endif

fail_dma:
    FreeMem(tsrc, dma_len * 3);
fail_src_alloc:

    show_test_state("DMA test:", rc);
    return (rc);
}

/*
 * test_dma_copy
 * -------------
 * This function will drive a DMA copy from/to Amiga main memory. Since we
 * can't yet trust DMA as working reliably (the controller might have
 * floating address lines), the code tries to protect Amiga memory at bit
 * flip addresses. The destination address is used to determine the
 * addresses which need to be protected. Single and double bit flip addresses
 * are either allocated or copied / restored.
 *
 * I think only 4-byte alignment is required by the 53C710 DMA controller,
 * but 16-byte alignment might be best for burst optimization. Need to test
 * this.
 *
 * XXX: CTEST8.3 FLF (Flush DMA FIFO) may be used to write data in DMA FIFO
 *      to address in DNAD.
 *      CTEST5.DMAWR controls direction of the transfer.
 */
static int
test_dma_copy(void)
{
#define DMA_LEN_BIT 12 // 4K DMA
    uint     dma_len = BIT(DMA_LEN_BIT);
    uint     cur_dma_len = 4;
    int      rc = 0;
    int      pass;
    int      pos;
    int      bit1;
    int      bit2;
    int      bf_mismatches = 0;
    int      bf_copies = 0;
    int      bf_buffers = 0;
    APTR    *src;
    APTR    *dst;
    APTR    *dst_buf;
    ULONG    buf_handled;
typedef uint32_t addrs_array[32];
    addrs_array *bf_addr;
    addrs_array *bf_mem;
    uint8_t bf_flags[32][32];
#define BF_FLAG_COPY    0x01
#define BF_FLAG_CORRUPT 0x02

    show_test_state("DMA copy:", -1);

    srand32(time(NULL));
    memset(bf_flags, 0, sizeof (bf_flags));

    src = AllocMem_aligned(dma_len, 16);
    if (src == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    dst_buf = AllocMem_aligned(dma_len * 3, 16);
    if (dst_buf == NULL) {
        printf("Failed to allocate dst buffer\n");
        rc = 1;
        goto fail_dst_alloc;
    }

    /* Land DMA in the middle of the buffer */
    dst = (APTR) ((uint32_t) dst_buf + dma_len);

#define BFADDR_SIZE (32 * 32 * sizeof (uint32_t))
    bf_addr = AllocMem(BFADDR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    bf_mem = AllocMem(BFADDR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if ((bf_addr == NULL) || (bf_mem == NULL)) {
        printf("Failed to allocate protection array\n");
        rc = 1;
        goto fail_bfaddr_alloc;
    }
#if 0
printf("src=%p dst=%p dst_buf=%p bf_addr=%p bf_mem=%p\n", src, dst, dst_buf, bf_addr, bf_mem);
#endif

    if (runtime_flags & FLAG_DEBUG)
        printf("\nDMA src=%08x dst=%08x len=%x\n",
               (uint32_t) src, (uint32_t) dst, dma_len);

    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (bit1 == bit2)
                bf_addr[bit1][bit2] = ((uint32_t) dst) ^ BIT(bit1);
            else
                bf_addr[bit1][bit2] = ((uint32_t) dst) ^ BIT(bit1) ^ BIT(bit2);
            bf_mem[bit1][bit2] =
                (uint32_t) AllocAbs(dma_len, (APTR) bf_addr[bit1][bit2]);
            if (bf_mem[bit1][bit2] == 0) {
                bf_mem[bit1][bit2] = (uint32_t) AllocMem(dma_len, MEMF_PUBLIC);
                bf_flags[bit1][bit2] |= BF_FLAG_COPY;
                bf_copies++;
            } else {
                /* Got target memory -- wipe it */
                memset((APTR) bf_mem[bit1][bit2], 0, dma_len);
                bf_buffers++;
            }
        }
    }
    if (runtime_flags & FLAG_DEBUG) {
        printf("Bit flip addrs:\n");
        for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
            for (bit2 = bit1; bit2 < 32; bit2++)
                printf(" %08x", bf_addr[bit1][bit2]);
            printf("\n");
        }
    }
// printf("1[%x]\n", bf_mem[0][0]);

    dma_init_siop();
    Forbid();
    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (bf_flags[bit1][bit2] & BF_FLAG_COPY) {
                memcpy((APTR)bf_mem[bit1][bit2], (APTR)bf_addr[bit1][bit2], dma_len);
            }
        }
    }
    for (pass = 0; pass < 32; pass++) {
        memset(dst, 0, cur_dma_len);
        for (pos = 0; pos < cur_dma_len; pos += 4) {
            *(uint32_t *) ((uint32_t) src + pos) = rand32();
        }
        buf_handled = cur_dma_len;
        CachePreDMA((APTR) dst, &buf_handled, DMA_ReadFromRAM);
        CachePostDMA((APTR) dst, &buf_handled, DMA_ReadFromRAM);

        CachePreDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
        CachePreDMA((APTR) dst, &buf_handled, 0);

        a4091_reset();
        rc = dma_mem_to_mem((uint32_t) src, (uint32_t) dst, cur_dma_len);
        CachePostDMA((APTR) dst, &buf_handled, 0);
        CachePostDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            /* DMA operation failed */
            break;
        }

#if 0
printf("4[%x] %x\n", bf_mem[0][0], *(uint32_t *)src);
printf("src=%p dst=%p dst_buf=%p bf_addr=%p bf_mem=%p\n", src, dst, dst_buf, bf_addr, bf_mem);
#endif

        /* Verify data landed where it was expected */
        for (pos = 0; pos < cur_dma_len; pos += 4) {
            uint32_t svalue = *(uint32_t *) ((uint32_t) src + pos);
            uint32_t dvalue = *(uint32_t *) ((uint32_t) dst + pos);
            if (svalue != dvalue) {
                if (rc == 0) {
                    printf("\nDMA src=%08x dst=%08x len=%x\n",
                           (uint32_t) src, (uint32_t) dst, cur_dma_len);
                }
                if ((rc < 5) || (runtime_flags & FLAG_DEBUG)) {
                    printf(" Addr %08x value %08x != expected %08x "
                           "(diff %08x)\n",
                           (uint32_t) dst + pos, dvalue, svalue,
                           dvalue ^ svalue);
                }
                rc++;
            }
        }

        /*
         * If any part of the landing area is wrong, look for the missing
         * data elsewhere in memory (address line floating or shorted).
         */
        if (rc > 0) {
            if (rc > 5)
                printf("...");
            printf("%d total miscompares\n", rc);

            /* Miscompare -- attempt to locate the data elsewhere in memory */


            /*
             * For now, this code will just display all address blocks
             * which differ from the before-test version.
             */
            for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
                for (bit2 = bit1; bit2 < 32; bit2++) {
                    if (bf_flags[bit1][bit2] & BF_FLAG_COPY) {
                        if (memcmp((APTR)bf_mem[bit1][bit2],
                                   (APTR)bf_addr[bit1][bit2], dma_len) != 0) {
                            bf_flags[bit1][bit2] |= BF_FLAG_CORRUPT;
                            if (bf_mismatches++ == 0)
                                printf("Modified RAM addresses: ");
                            printf("<%x>", bf_addr[bit1][bit2]);
                        }
                    } else if (mem_not_zero(bf_mem[bit1][bit2], dma_len)) {
                        if (bf_mismatches++ == 0)
                            printf("Modified RAM addresses: ");
                        printf(">%x<", bf_addr[bit1][bit2]);
                    }
                }
            }
            if (bf_mismatches != 0)
                printf("\n");
        }

        if (rc != 0)
            break;

        cur_dma_len <<= 1;
        if (cur_dma_len >= dma_len)
            cur_dma_len = dma_len;
    }
    Permit();
    if (0)
        printf("BF buffers=%d copies=%d mismatches=%d\n",
               bf_buffers, bf_copies, bf_mismatches);

    /* Deallocate protected memory */
    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (bf_mem[bit1][bit2] != 0) {
#ifdef DEBUG_ALLOC
                if (((uint32_t) bf_mem[bit1][bit2] >= 0x07000000) &&
                    ((uint32_t) bf_mem[bit1][bit2] < 0x08000000)) {
                    FreeMem((APTR) bf_mem[bit1][bit2], dma_len);
                } else {
                    printf("!%x!", bf_mem[bit1][bit2]);
                }
#else
                FreeMem((APTR) bf_mem[bit1][bit2], dma_len);
#endif
            }
        }
    }

fail_bfaddr_alloc:
    if (bf_mem != NULL)
        FreeMem(bf_mem, BFADDR_SIZE);
    if (bf_addr != NULL)
        FreeMem(bf_addr, BFADDR_SIZE);
    FreeMem(dst_buf, dma_len * 3);
fail_dst_alloc:
    FreeMem(src, dma_len);

fail_src_alloc:
    show_test_state("DMA copy:", rc);
    return (rc);
}

/*
 * test_dma_copy_perf
 * ------------------
 * This test benchmarks repeated 64K DMA from and to CPU memory.
 * The expected performance is right around 5.2MB/sec. There is
 * currently no range checking done on the measured performance.
 */
static int
test_dma_copy_perf(void)
{
    uint      dma_len = 64 << 10;  // DMA maximum is 16MB - 1
    int       rc = 0;
    int       pass;
    int       total_passes = 0;
    volatile APTR     src;
    volatile APTR     dst;
    uint64_t  tick_start;
    uint64_t  tick_end;
    ULONG     buf_handled;

    show_test_state("DMA copy perf:", -1);

    a4091_reset();

    src = AllocMem_aligned(dma_len, 64);
    if (src == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    dst = AllocMem_aligned(dma_len, 64);
    if (dst == NULL) {
        printf("Failed to allocate dst buffer\n");
        rc = 1;
        goto fail_dst_alloc;
    }
#if 0
    *(uint32_t *) ((uint32_t)src + dma_len - 4) = 0x12345678;
    *(uint32_t *) ((uint32_t)dst + dma_len - 4) = 0;
    CACHE_LINE_WRITE((uint32_t) src, 0x10);
    CACHE_LINE_WRITE((uint32_t) dst, 0x10);
#endif
    buf_handled = dma_len;
    CachePreDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
    CachePreDMA(dst, &buf_handled, 0);

    if (runtime_flags & FLAG_DEBUG)
        printf("\nDMA src=%08x dst=%08x len=%x\n",
               (uint32_t) src, (uint32_t) dst, dma_len);

    a4091_reset();
    dma_init_siop();
    tick_start = read_system_ticks();
run_some_more:
    for (pass = 0; pass < 16; pass++) {
        total_passes++;
        if (dma_mem_to_mem_quad(src, dst, dma_len, pass == 0)) {
            rc = 1;
            break;
        }
    }
    if (rc == 0) {
        tick_end = read_system_ticks();
        uint64_t ticks = tick_end - tick_start;
        uint64_t total_kb = total_passes * (dma_len / 1024) * 2 * 4;
        /*                          2=R/w, 4=4 transfers in script */
        const char *passfail = "PASS";
        if (ticks < 10)
            goto run_some_more;

        printf("%s: %u KB in %d ticks",
                passfail, (uint32_t) total_kb, (uint32_t) ticks);
        if (ticks == 0)
            ticks = 1;
        printf(" (%u KB/sec)\n",
                (uint32_t) (total_kb * TICKS_PER_SECOND / ticks));
    }
    CachePostDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
    CachePostDMA(dst, &buf_handled, 0);

    FreeMem((APTR *) dst, dma_len);
fail_dst_alloc:
    FreeMem((APTR *) src, dma_len);

fail_src_alloc:
    if (rc != 0)
        show_test_state("DMA copy perf:", rc);
    return (rc);
}


/*
 * test_card
 * ---------
 * Tests the specified A4091 card.
 */
static int
test_card(uint test_flags)
{
    int rc = 0;

    if (test_flags == 0)
        test_flags = -1;

    /* Take over 53C710 interrupt handling and register state */
    a4091_state_takeover();

    if ((rc == 0) && (test_flags & BIT(0)))
        rc = test_device_access();

    check_break();
    if ((rc == 0) && (test_flags & BIT(1)))
        rc = test_register_access();

    check_break();
    if ((rc == 0) && (test_flags & BIT(2)))
        rc = test_dma_fifo();

    check_break();
    if ((rc == 0) && (test_flags & BIT(3)))
        rc = test_scsi_fifo();

    check_break();
    if ((rc == 0) && (test_flags & BIT(4)))
        rc = test_dma();

    check_break();
    if ((rc == 0) && (test_flags & BIT(5)))
        rc = test_dma_copy();

    check_break();
    if ((rc == 0) && (test_flags & BIT(6)))
        rc = test_dma_copy_perf();

#if 0
    /* Loopback test not implemented yet */
    check_break();
    if ((rc == 0) && (test_flags & BIT(7)))
        rc = test_loopback();
#endif

    check_break();
    if ((rc == 0) && (test_flags & BIT(7)))
        rc = test_scsi_pins();

    a4091_state_restore();
    return (rc);
}

/*
 * a4901_list
 * ----------
 * Display list of all A4091 cards found during autoconfig.
 */
static int
a4091_list(uint32_t addr)
{
    struct Library        *ExpansionBase;
    struct ConfigDev      *cdev = NULL;
    struct CurrentBinding  cbind;
    int                    count = 0;
    int                    did_header = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return (1);
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            if (((addr > 0x10) && ((uint32_t) cdev->cd_BoardAddr != addr)) ||
                ((addr <= 0x10) && (count != addr))) {
                count++;
                continue;
            }
            if (did_header == 0) {
                did_header++;
                printf("  Index Address  Size     Flags\n");
            }
            printf("  %-3d   %08x %08"PRIx32,
                   count, (uint32_t) cdev->cd_BoardAddr, cdev->cd_BoardSize);
            if (cdev->cd_Flags & CDF_SHUTUP)
                printf(" ShutUp");
            if (cdev->cd_Flags & CDF_CONFIGME)
                printf(" ConfigMe");
            if (cdev->cd_Flags & CDF_BADMEMORY)
                printf(" BadMemory");
            cbind.cb_ConfigDev = cdev;
            if (GetCurrentBinding(&cbind, sizeof (cbind)) >= sizeof (cbind)) {
                printf(" Bound");
                if (cbind.cb_FileName != NULL)
                    printf(" to %s", cbind.cb_FileName);
                if (cbind.cb_ProductString != NULL)
                    printf(" prod %s", cbind.cb_ProductString);
            }
            printf("\n");
            count++;
        }
    } while (cdev != NULL);

    if (count == 0)
        printf("No A4091 cards detected\n");
    else if (did_header == 0)
        printf("Specified card %x not detected\n", addr);

    CloseLibrary(ExpansionBase);
    return (count == 0);
}


/*
 * a4091_find
 * ----------
 * Locates the specified A4091 in the system (by autoconfig order).
 */
static uint32_t
a4091_find(uint32_t pos)
{
    struct Library   *ExpansionBase;
    struct ConfigDev *cdev  = NULL;
    uint32_t          addr  = -1;  /* Default to not found */
    int               count = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return (-1);
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            if (pos == count) {
                addr = (uint32_t) cdev->cd_BoardAddr;
                break;
            }
            count++;
        }
    } while (cdev != NULL);

    CloseLibrary(ExpansionBase);

    return (addr);
}

/*
 * enforcer_check
 * --------------
 * Verifies enforcer is not running.
 */
static int
enforcer_check(void)
{
    Forbid();
    if (FindTask("� Enforcer �") != NULL) {
        /* Enforcer is running */
        Permit();
        printf("Enforcer is present.  First use \"enforcer off\" to "
               "disable enforcer.\n");
        return (1);
    }
    if (FindTask("� MuForce �") != NULL) {
        /* MuForce is running */
        Permit();
        printf("MuForce is present.  First use \"muforce off\" to "
               "disable MuForce.\n");
        return (1);
    }
    Permit();
    return (0);
}

/*
 * nextarg
 * -------
 * Returns the next argument in the argv list (argument in the list is
 * automatically set to NULL).
 */
static char *
nextarg(int argc, char **argv, int argnum)
{
    for (; argnum < argc; argnum++) {
        char *next = argv[argnum];
        if (next == NULL)
            continue;
        argv[argnum] = NULL;
        return (next);
    }
    return (NULL);
}

/*
 * usage
 * -----
 * Displays program usage.
 */
static void
usage(void)
{
    printf("%s\n\n"
           "This tool will test an installed A4091 SCSI controller for "
           "correct operation.\n"
           "Options:\n"
           "\t-a  specify card address (slot or physical address): <addr>\n"
           "\t-c  decode device autoconfig area\n"
           "\t-d  enable debug output\n"
           "\t-D  perform DMA from/to Amiga memory: <src> <dst> <len>\n"
           "\t-f  ignore fact enforcer is present\n"
           "\t-h  display this help text\n"
           "\t-k  kill (disable) active C= A4091 device driver\n"
           "\t-L  loop until failure\n"
           "\t-P  probe and list all detected A4091 cards\n"
           "\t-r  display NCR53C710 registers\n"
           "\t-s  decode device external switches\n"
           "\t-t  test card\n",
           version + 7);
}

/*
 * main
 * ----
 * Parse and execute arguments.
 */
int
main(int argc, char **argv)
{
    int      arg;
    int      rc             = 0;
    int      flag_config    = 0;  /* Decode device autoconfig area */
    int      flag_dma       = 0;  /* Copy memory using 53C710 DMA engine */
    int      flag_force     = 0;  /* Ignore the fact that enforcer is present */
    int      flag_loop      = 0;  /* Loop all tests until failure */
    int      flag_kill      = 0;  /* Kill active A4091 device driver */
    int      flag_list      = 0;  /* List all A4091 cards found */
    int      flag_regs      = 0;  /* Decode device registers */
    int      flag_switches  = 0;  /* Decode device external switches */
    int      flag_test      = 0;  /* Test card */
    uint     test_flags     = 0;  /* Test flags (0-9) */
    uint32_t addr           = 0;  /* Card physical address or index number */
    uint32_t dma[3];              /* DMA source, destination, length */

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (ptr == NULL)
            continue;  // Already grabbed this argument
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9':
                        test_flags |= BIT(*ptr - '0');
                        break;
                    case 'a': {
                        /* card address */
                        int pos = 0;

                        if (++arg >= argc) {
                            printf("You must specify an address\n");
                            exit(1);
                        }
                        if ((sscanf(argv[arg], "%x%n", &addr, &pos) != 1) ||
                            (pos == 0)) {
                            printf("Invalid card address %s specified\n", ptr);
                            exit(1);
                        }
                        break;
                    }
                    case 'c':
                        flag_config = 1;
                        break;
                    case 'd':  /* debug */
                        if (runtime_flags & FLAG_DEBUG)
                            runtime_flags |= FLAG_MORE_DEBUG;
                        else
                            runtime_flags |= FLAG_DEBUG;
                        break;
                    case 'D': {  /* DMA */
                        char *s[3];
                        int  i;
                        int  pos;
                        static const char * const which[] = {
                            "src", "dst", "len"
                        };
                        flag_dma = 1;

                        for (i = 0; i < 3; i++) {
                            s[i] = nextarg(argc, argv, arg + 1);
                            if (s[i] == NULL) {
                                printf("Command requires <src> <dst> <len>\n");
                                exit(1);
                            }
                            if ((sscanf(s[i], "%x%n", &dma[i], &pos) != 1) ||
                                (pos == 0)) {
                                printf("Invalid DMA %s %s specified\n",
                                        which[i], s[i]);
                                exit(1);
                            }
                        }
                        break;
                    }
                    case 'h':
                        usage();
                        exit(0);
                    case 'f':
                        flag_force = 1;
                        break;
                    case 'k':
                        flag_kill = 1;
                        break;
                    // case 'l':  // Reserved for loop count
                    case 'L':
                        flag_loop = 1;
                        break;
                    case 'P':
                        flag_list = 1;
                        break;
                    case 'r':
                        flag_regs = 1;
                        break;
                    case 's':
                        flag_switches = 1;
                        break;
                    case 't':
                        flag_test = 1;
                        break;
                    default:
                        printf("Unknown -%s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else {
            printf("Unknown argument %s\n", ptr);
            usage();
            exit(1);
        }
    }

    if (flag_list)
        rc += a4091_list(addr);

    if (!(flag_config | flag_dma | flag_regs | flag_switches | flag_test |
          flag_kill)) {
        if (flag_list)
            exit(rc);
        usage();
        exit(1);
    }

    if (!flag_force && enforcer_check())
        exit(1);

    if (addr < 0x10)
        a4091_base = a4091_find(addr);
    else
        a4091_base = addr;

    if (a4091_base == -1) {
        printf("No A4091 cards detected\n");
        exit(1);
    }
    printf("A4091 at 0x%08x\n", a4091_base);
    a4091_save.addr = a4091_base;  // Base address for current card

    if (flag_kill)
        rc += kill_driver();

    do {
        if (flag_config)
            rc += decode_autoconfig();
        if (flag_regs)
            rc += decode_registers();
        if (flag_switches)
            rc += decode_switches();
        if (flag_dma) {
            dma_init_siop();
            rc += dma_mem_to_mem(dma[0], dma[1], dma[2]);
        }
        if (flag_test)
            rc += test_card(test_flags);
        check_break();
    } while ((rc == 0) && flag_loop);

    return (rc);
}
