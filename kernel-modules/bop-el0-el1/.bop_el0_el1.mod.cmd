savedcmd_bop_el0_el1.mod := printf '%s\n'   bop_el0_el1.o | awk '!x[$$0]++ { print("./"$$0) }' > bop_el0_el1.mod
