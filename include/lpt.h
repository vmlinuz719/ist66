#ifndef _LPT_
#define _LPT_

#include <stdio.h>

#include "cpu.h"

void init_lpt(acr7k_cu_t *cpu, int id, int irq, FILE *fd);
void init_lpt_ex(acr7k_cu_t *cpu, int id, int irq, char *fname);

#endif
