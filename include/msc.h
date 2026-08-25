#ifndef _MSC_
#define _MSC_

#include "cpu.h"

void init_msch(acr7k_cu_t *cpu, int id, int irq);

int sc_attach(acr7k_cu_t *cpu, int id, int sc_id);
int sc_detach(acr7k_cu_t *cpu, int id, int sc_id);

#endif