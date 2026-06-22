#ifndef _PCH_
#define _PCH_

#include "cpu.h"

void init_pch(acr7k_cu_t *cpu, int id, int irq);
void init_pch_ex(acr7k_cu_t *cpu, int id, int irq, char *fname);

#endif