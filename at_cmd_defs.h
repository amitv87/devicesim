#ifndef REG_CMD
#define REG_CMD(...)
#endif

/*REG_CMD(<cmd>, <prefix>, tst, get, set, exe)*/

REG_CMD(A,        "",  0, 0, 0, 1)
REG_CMD(D,        "",  0, 0, 0, 1)
REG_CMD(E,        "",  0, 0, 0, 1)
REG_CMD(H,        "",  0, 0, 0, 1)
REG_CMD(I,        "",  0, 0, 0, 1)

REG_CMD(CBC,      "+", 1, 0, 0, 1)
REG_CMD(CGSN,     "+", 0, 0, 0, 1)
REG_CMD(CFUN,     "+", 1, 1, 1, 0)
REG_CMD(CPIN,     "+", 1, 1, 1, 0)
REG_CMD(CSPN,     "+", 0, 1, 0, 1)
REG_CMD(CCID,     "+", 1, 0, 0, 1)
REG_CMD(CENG,     "+", 1, 1, 1, 0)
REG_CMD(CTZU,     "+", 1, 1, 1, 0)

REG_CMD(CREG,     "+", 1, 1, 1, 0)
REG_CMD(CGREG,    "+", 1, 1, 1, 0)
REG_CMD(CSQ,      "+", 1, 0, 0, 1)
REG_CMD(CCLK,     "+", 1, 1, 1, 0)

REG_CMD(CRSM,     "+", 1, 0, 1, 0)

REG_CMD(CMUX,     "+", 1, 1, 1, 0)

REG_CMD(CLAC,     "+", 0, 0, 0, 1)

REG_CMD(QSPN,     "+", 0, 1, 0, 1)
REG_CMD(QGSN,     "+", 0, 0, 0, 1)
REG_CMD(QENG,     "+", 1, 1, 1, 0)
REG_CMD(QCCID,    "+", 1, 0, 0, 1)

REG_CMD(CGATT,    "+", 1, 1, 1, 0)
REG_CMD(CGDCONT,  "+", 1, 1, 1, 0)

#undef REG_CMD
