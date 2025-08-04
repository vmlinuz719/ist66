#ifndef _PCH_
#define _PCH_

#include "cpu.h"

void init_pch(ist66_cu_t *cpu, int id, int irq);
void init_pch_ex(ist66_cu_t *cpu, int id, int irq, char *fname);

#endif