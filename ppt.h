#ifndef _PPT_
#define _PPT_

#include "cpu.h"

void init_ppt(ist66_cu_t *cpu, int id, int irq);
void init_ppt_ex(ist66_cu_t *cpu, int id, int irq, char *fname);

#endif