#!/bin/sh
# System clock from the RTC at start, RTC from the system clock at stop. The RTC holds UTC.
case "$1" in
    start)
        hwclock -s -u
        ;;
    stop)
        hwclock -w -u
        ;;
    restart|reload) ;;
    *) echo "Usage: $0 {start|stop|restart|reload}" >&2; exit 1 ;;
esac
exit 0
