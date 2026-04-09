        ; Proof of concept using m4 for assembler macros

define(dsncpy,`
            ld          xy, ($1)
            st          xy, ($2)
            incldb      ac, $1, 7
            incstb      ac, $2, 7
            addi.rn     xy, xy, -1
            jmp         .-3
')

            origin      1024

test:       dw          0

            ldea        x0, .src
            ldea        x1, .dst

            dsncpy(x0, x1)

            rets

src:        dsn         "Hello World", 13, 10
dst:        bss         16
