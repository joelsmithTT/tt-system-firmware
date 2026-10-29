// L1 layout shared by the BRISC program and the host driver.
#define PARAM_N      0x8000u // tokens to send
#define PARAM_MARKER 0x8004u // data[0] of every record
#define PARAM_SYNC   0x8008u // nonzero: wait for each write to complete before the next
#define DONE         0x8010u // BRISC writes N here when finished
#define TOKBUF       0x9000u // token i staged at TOKBUF + 64 * i
#define RECORDS      0x20000u // record i at RECORDS + 32 * i
#define MAX_TOKENS   128u
#define RECORD_MAGIC 0x54455654u // "TEVT"
#define ARC_FIFO     0x800B0000u
#define ARC_COORD    8u // (8, 0)
