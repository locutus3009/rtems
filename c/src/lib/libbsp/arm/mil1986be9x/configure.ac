AC_PREREQ([2.69])
AC_INIT([rtems-c-src-lib-libbsp-arm-mil1986be9x],[_RTEMS_VERSION],[https://devel.rtems.org/newticket])
AC_CONFIG_SRCDIR([bsp_specs])
RTEMS_TOP(../../../../../..)

RTEMS_CANONICAL_TARGET_CPU
AM_INIT_AUTOMAKE([no-define nostdinc foreign 1.12.2])
RTEMS_BSP_CONFIGURE

RTEMS_PROG_CC_FOR_TARGET
RTEMS_CANONICALIZE_TOOLS
RTEMS_PROG_CCAS

RTEMS_CHECK_NETWORKING
AM_CONDITIONAL(HAS_NETWORKING,test "$HAS_NETWORKING" = "yes")

RTEMS_BSPOPTS_SET([BSP_SMALL_MEMORY],[*],[])
RTEMS_BSPOPTS_HELP([BSP_SMALL_MEMORY],[disable testsuite samples with high memory demands])

RTEMS_BSPOPTS_SET([MIL1986BE9X_TICKS_PER_SECOND],[*],[1000])
RTEMS_BSPOPTS_HELP([MIL1986BE9X_TICKS_PER_SECOND],[ticks per second in Hz])

RTEMS_BSPOPTS_SET([MIL1986BE9X_INPUT_CLOCK],[*],[8000000])
RTEMS_BSPOPTS_HELP([MIL1986BE9X_INPUT_CLOCK],[external clock in Hz])

RTEMS_BSPOPTS_SET([MIL1986BE9X_SYSTEM_CLOCK],[*],[48000000])
RTEMS_BSPOPTS_HELP([MIL1986BE9X_SYSTEM_CLOCK],[system clock in Hz])

RTEMS_BSPOPTS_SET([MIL1986BE9X_UART_BAUD],[*],[115200])
RTEMS_BSPOPTS_HELP([MIL1986BE9X_UART_BAUD],[baud for UARTs])

RTEMS_BSPOPTS_SET([MIL1986BE9X_USE_ERAM],[*],[1])
RTEMS_BSPOPTS_HELP([MIL1986BE9X_USE_ERAM],[use external RAM])

RTEMS_BSP_CLEANUP_OPTIONS(0, 0)
RTEMS_BSP_LINKCMDS

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
