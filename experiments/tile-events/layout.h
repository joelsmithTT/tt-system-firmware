// L1 layout shared by the BRISC program and the host driver.
#define PARAM_N      0x8000u // tokens to send
#define PARAM_MARKER 0x8004u // data[0] of every record
#define PARAM_SYNC   0x8008u // nonzero: wait for each write to complete before the next
#define PARAM_PMM    0x800Cu // nonzero: announce through the PMM mailbox, not the MSI catcher
#define DONE         0x8010u // BRISC writes N here when finished, or DONE_STUCK | i
#define DONE_STUCK   0x80000000u // PMM mode: record i was never acked
#define TOKBUF       0x9000u // token i staged at TOKBUF + 64 * i
#define RECORDS      0x20000u // record i at RECORDS + 512 * i (PMM needs 512-byte alignment)
#define RECORD_STRIDE 512u
#define MAX_TOKENS   128u
#define RECORD_MAGIC 0x54455654u // "TEVT"
#define ARC_FIFO     0x800B0000u
#define ARC_PMM      0x80400000u
#define ARC_COORD    8u // (8, 0)
#define ACK_SPINS    2000000u // PMM mode: polls of a record's magic before giving up
