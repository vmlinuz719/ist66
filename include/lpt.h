#ifndef _LPT_
#define _LPT_

#include "cpu.h"

void init_lpt(ist66_cu_t *cpu, int id, int irq, FILE *fd);
void init_lpt_ex(ist66_cu_t *cpu, int id, int irq, char *fname);

#endif