#!/usr/bin/env bash
#
# dump-boot-regs.sh <ip> -- read the SoC registers a bootloader programs, from the
# running kernel, one "0xADDR=0xVALUE" line each, sorted by address.
#
# WHY: the acceptance check for our own SPL + U-Boot is "does the kernel see the
# same hardware setup the vendor bootloader left?".  Dump once per bootloader and
# diff the two files; every differing line is either kernel-owned state (timers,
# counters, status bits) or a setting only the bootloader makes.
#
# READ-ONLY.  Every access is busybox `devmem <addr> 32` -- two arguments, a read.
# There is no three-argument form anywhere in this file, and tests/dump_boot_regs_test.sh
# fails if one appears.  The script takes no register or value argument, so it
# cannot be pointed at a write.
#
# ONE devmem PER REGISTER.  Reading a register of an unclocked module bus-faults
# and kills devmem (seen on GPIO6).  Each read is its own process, so a fault costs
# that line only and is recorded as "0xADDR=FAULT" instead of aborting the dump.
# All reads run in ONE ssh session (a loop on the unit), not one ssh per register.
#
# -- Where the addresses come from --------------------------------------------
# U-Boot v2026.10, arch/arm/include/asm/arch-omap3/ (the OMAP3 TRM-derived headers):
#   omap.h    OMAP34XX_CTRL_BASE 0x48002000 (L4 core 0x48000000 + 0x2000),
#             OMAP34XX_SDRC_BASE 0x6D000000, OMAP34XX_GPMC_BASE 0x6E000000
#   cpu.h     struct sdrc / sdrc_cs / sdrc_actim : sysconfig 0x10, status 0x14,
#             cs_cfg 0x40, sharing 0x44, dlla_ctrl 0x60, dlla_status 0x64, power 0x70,
#             cs[2] at 0x80 / 0xB0 (mcfg +0, mr +4, emr2 +0xC, rfr_ctrl +0x24,
#             manual +0x28), actim ctrla/ctrlb at 0x9C/0xA0 and 0xC4/0xC8
#             (SDRC_ACTIM_CTRL0_BASE / SDRC_ACTIM_CTRL1_BASE)
#             struct prcm (PRCM_BASE 0x48004000): every clken/clksel/fclken/iclken
#             member, offsets as the comments in the struct
#             struct prm (PRM_BASE 0x48306000): clksel 0xD40, clksrc_ctrl 0x1270
#             (= PRM_RSTCTRL 0x48307250 + 0x20)
#             GPMC_CONFIG_CS0 0x60 and GPMC_CONFIG1..7 0x00..0x18
#   include/linux/mtd/omap_gpmc.h   struct gpmc: sysconfig 0x10, irqenable 0x1C,
#             timeout_control 0x40, config 0x50, status 0x54, cs[8] at 0x60, stride 0x30
#   mux.h     CP() padconf offsets; the padconf windows are the 152 words that
#             uboot/rw20-board.patch replays (0x48002030-0x48002264,
#             0x48002A00-0x48002A24)
# PRM_VOLTCTRL is not in the U-Boot headers.  Kernel 4.14.52,
#   arch/arm/mach-omap2/prm3xxx.h OMAP3_PRM_VOLTCTRL_OFFSET 0x60 on OMAP3430_GR_MOD
#   (prcm-common.h 0xa00) over OMAP3430_PRM_BASE 0x48306800 (omap34xx.h) = 0x48307260.
#
# -- Reading a diff -----------------------------------------------------------
# Take both dumps with the backlight in the SAME state.  CM_FCLKEN_PER 0x48005000
# bit 10 (EN_GPT9, 0x400) follows the backlight [inferred: GPT9 is the backlight PWM;
# seen on two units, backlight 100 -> 0x400, backlight 0 -> bit clear].  A dump taken
# just after boot, while the backlight is still lit, differs from one taken with the
# backlight at 0 in that bit alone.  With the backlight at 0, GPT9 is unclocked and
# its own registers bus-fault, which the dump records as FAULT (measured, 2026-10-12:
# 251 registers, vendor chain vs build H, empty diff in the same backlight state).
# A unit whose root filesystem has no devmem applet reports EVERY register as FAULT;
# a dump that is all FAULT is a missing tool, not a hardware result.  Check the fault
# count on the last line (stderr) before diffing.
#
# Usage: uboot/tools/dump-boot-regs.sh <ip> > regs.txt
#        uboot/tools/dump-boot-regs.sh --list     (names + addresses, no device)

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null  # not followed: shellcheck resolves ../../ against the repo root in the gate
. "$REPO_ROOT/lib/arca-ssh.sh"


# add <name> <base> <offset> ...  -- appends "name addr" rows (addr as 0x%08x)
REG_ROWS=()
add() {
    local name="$1" base="$2" off
    shift 2
    for off in "$@"; do
        REG_ROWS+=("$(printf '%s 0x%08x' "$name" $((base + off)))")
    done
}

build_rows() {
    local a

    # padconf: 32-bit words, two pads each
    for ((a = 0x30; a <= 0x264; a += 4)); do add "PADCONF" 0x48002000 "$a"; done
    for ((a = 0xA00; a <= 0xA24; a += 4)); do add "PADCONF_WKUP" 0x48002000 "$a"; done

    # SDRC (OMAP34XX_SDRC_BASE)
    local sdrc=0x6D000000
    add SDRC_SYSCONFIG   $sdrc 0x10
    add SDRC_STATUS      $sdrc 0x14
    add SDRC_CS_CFG      $sdrc 0x40
    add SDRC_SHARING     $sdrc 0x44
    add SDRC_DLLA_CTRL   $sdrc 0x60
    add SDRC_DLLA_STATUS $sdrc 0x64
    add SDRC_POWER       $sdrc 0x70
    add SDRC_MCFG_0      $sdrc 0x80
    add SDRC_MR_0        $sdrc 0x84
    add SDRC_EMR2_0      $sdrc 0x8C
    add SDRC_ACTIM_CTRLA_0 $sdrc 0x9C
    add SDRC_ACTIM_CTRLB_0 $sdrc 0xA0
    add SDRC_RFR_CTRL_0  $sdrc 0xA4
    add SDRC_MANUAL_0    $sdrc 0xA8
    add SDRC_MCFG_1      $sdrc 0xB0
    add SDRC_MR_1        $sdrc 0xB4
    add SDRC_EMR2_1      $sdrc 0xBC
    add SDRC_ACTIM_CTRLA_1 $sdrc 0xC4
    add SDRC_ACTIM_CTRLB_1 $sdrc 0xC8
    add SDRC_RFR_CTRL_1  $sdrc 0xD4
    add SDRC_MANUAL_1    $sdrc 0xD8

    # CM (PRCM_BASE 0x48004000, struct prcm)
    local cm=0x48004000
    add CM_FCLKEN_IVA2      $cm 0x000
    add CM_CLKEN_PLL_IVA2   $cm 0x004
    add CM_IDLEST_PLL_IVA2  $cm 0x024
    add CM_CLKSEL1_PLL_IVA2 $cm 0x040
    add CM_CLKSEL2_PLL_IVA2 $cm 0x044
    add CM_CLKEN_PLL_MPU    $cm 0x904
    add CM_IDLEST_PLL_MPU   $cm 0x924
    add CM_CLKSEL1_PLL_MPU  $cm 0x940
    add CM_CLKSEL2_PLL_MPU  $cm 0x944
    add CM_FCLKEN1_CORE     $cm 0xA00
    add CM_FCLKEN3_CORE     $cm 0xA08
    add CM_ICLKEN1_CORE     $cm 0xA10
    add CM_ICLKEN2_CORE     $cm 0xA14
    add CM_ICLKEN3_CORE     $cm 0xA18
    add CM_CLKSEL_CORE      $cm 0xA40
    add CM_FCLKEN_GFX       $cm 0xB00
    add CM_ICLKEN_GFX       $cm 0xB10
    add CM_CLKSEL_GFX       $cm 0xB40
    add CM_FCLKEN_WKUP      $cm 0xC00
    add CM_ICLKEN_WKUP      $cm 0xC10
    add CM_IDLEST_WKUP      $cm 0xC20
    add CM_CLKSEL_WKUP      $cm 0xC40
    add CM_CLKEN_PLL        $cm 0xD00
    add CM_CLKEN2_PLL       $cm 0xD04
    add CM_IDLEST_CKGEN     $cm 0xD20
    add CM_IDLEST2_CKGEN    $cm 0xD24
    add CM_CLKSEL1_PLL      $cm 0xD40
    add CM_CLKSEL2_PLL      $cm 0xD44
    add CM_CLKSEL3_PLL      $cm 0xD48
    add CM_CLKSEL4_PLL      $cm 0xD4C
    add CM_CLKSEL5_PLL      $cm 0xD50
    add CM_FCLKEN_DSS       $cm 0xE00
    add CM_ICLKEN_DSS       $cm 0xE10
    add CM_CLKSEL_DSS       $cm 0xE40
    add CM_FCLKEN_CAM       $cm 0xF00
    add CM_ICLKEN_CAM       $cm 0xF10
    add CM_CLKSEL_CAM       $cm 0xF40
    add CM_FCLKEN_PER       $cm 0x1000
    add CM_ICLKEN_PER       $cm 0x1010
    add CM_CLKSEL_PER       $cm 0x1040
    add CM_FCLKEN_USBHOST   $cm 0x1400
    add CM_ICLKEN_USBHOST   $cm 0x1410

    # PRM
    add PRM_CLKSEL         0x48306000 0xD40
    add PRM_VOLTCTRL       0x48307200 0x60
    add PRM_CLKSRC_CTRL    0x48307200 0x70

    # GPMC (OMAP34XX_GPMC_BASE): top-level, then CS0..CS3 CONFIG1..7
    local gpmc=0x6E000000 cs c
    add GPMC_SYSCONFIG       $gpmc 0x10
    add GPMC_IRQENABLE       $gpmc 0x1C
    add GPMC_TIMEOUT_CONTROL $gpmc 0x40
    add GPMC_CONFIG          $gpmc 0x50
    add GPMC_STATUS          $gpmc 0x54
    for cs in 0 1 2 3; do
        for c in 1 2 3 4 5 6 7; do
            add "GPMC_CONFIG${c}_CS${cs}" $gpmc $((0x60 + cs * 0x30 + (c - 1) * 4))
        done
    done
}

build_rows

for row in "${REG_ROWS[@]}"; do
    case "${row#* }" in
        0x[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) : ;;
        *) echo "dump-boot-regs.sh: bad address in table: $row" >&2; exit 2 ;;
    esac
done

case "${1:-}" in
    "")
        echo "usage: dump-boot-regs.sh <ip> | --list   (see the header of this file)" >&2
        exit 2 ;;
    -h|--help)
        sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
        exit 0 ;;
    --list)
        printf '%s\n' "${REG_ROWS[@]}" | LC_ALL=C sort -k2
        exit 0 ;;
esac

[ "$#" -eq 1 ] || { echo "dump-boot-regs.sh: exactly one argument (the unit address); this tool reads only" >&2; exit 2; }
IP="$1"
case "$IP" in
    *[!0-9a-zA-Z.-]*|-*) echo "dump-boot-regs.sh: not a host address: $IP" >&2; exit 2 ;;
esac

ADDRS=""
for row in "${REG_ROWS[@]}"; do ADDRS="$ADDRS ${row#* }"; done

# Runs on the unit (busybox ash).  One devmem per address; "devmem ADDR 32" is a read.
REMOTE="for a in$ADDRS; do v=\$(devmem \$a 32 2>/dev/null); if [ \$? -eq 0 ] && [ -n \"\$v\" ]; then echo \"\$a=\$v\"; else echo \"\$a=FAULT\"; fi; done"

# shellcheck disable=SC2029  # REMOTE is built here on purpose: the address list is client-side data
# Reachability and auth through the one gate (a probe: it never offers to install a key).
arca_ssh_probe "root@$IP" >/dev/null || { echo "dump-boot-regs.sh: cannot reach root@$IP over key-authenticated ssh" >&2; exit 1; }
OUT="$(ssh -o ConnectTimeout="$ARCA_SSH_CONNECT_TIMEOUT" "root@$IP" "$REMOTE")" || {
    echo "dump-boot-regs.sh: ssh to $IP failed" >&2
    exit 1
}
[ -n "$OUT" ] || { echo "dump-boot-regs.sh: no output from $IP" >&2; exit 1; }

printf '%s\n' "$OUT" | tr -d '\r' | LC_ALL=C sort
n=$(printf '%s\n' "$OUT" | grep -c '=')
f=$(printf '%s\n' "$OUT" | grep -c '=FAULT')
echo "dump-boot-regs.sh: $n registers, $f faults (expected ${#REG_ROWS[@]})" >&2
