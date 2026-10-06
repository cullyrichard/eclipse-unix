#!/usr/bin/env bash
# build-sim.sh -- build a private copy of the SIMH Eclipse simulator with
# the FPS-100 stand-in (nova_fpsx.c) at devices 054/055. ~/dev/simh-src
# is not modified. Output: tools/fps-standin/eclipse-fpsx
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
H="${SIMH:-$HOME/dev/simh-src}"; N="$H/NOVA"; w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
sed 's|^extern DEVICE plt_dev;|extern DEVICE plt_dev;\nextern DEVICE fpsa_dev, fpsb_dev;|; s|^    &plt_dev,|    \&plt_dev,\n    \&fpsa_dev,\n    \&fpsb_dev,|' \
  "$N/nova_sys.c" > "$w/nova_sys.c"
gcc -O2 -w -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DECLIPSE -DUSE_INT64 \
  -DSIM_COMPILER='"gcc"' -I "$N" -I "$H" -o "$here/eclipse-fpsx" \
  "$N/eclipse_cpu.c" "$N/eclipse_tt.c" "$w/nova_sys.c" "$here/nova_fpsx.c" "$N/nova_dkp.c" \
  "$N/nova_dsk.c" "$N/nova_lp.c" "$N/nova_mta.c" "$N/nova_plt.c" "$N/nova_pt.c" "$N/nova_clk.c" \
  "$N/nova_tt1.c" "$N/nova_qty.c" "$H/scp.c" "$H/sim_console.c" "$H/sim_fio.c" "$H/sim_timer.c" \
  "$H/sim_sock.c" "$H/sim_tmxr.c" "$H/sim_ether.c" "$H/sim_tape.c" "$H/sim_disk.c" \
  "$H/sim_serial.c" "$H/sim_video.c" "$H/sim_imd.c" "$H/sim_card.c" -lm -lpthread -ldl -lrt
echo "built $here/eclipse-fpsx"
