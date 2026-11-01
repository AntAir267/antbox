# Names a nvr_replay --prof file: functions by share of samples.
#   bash tools/nvr_replay/prof.sh BINARY PROF_FILE [TOP]
BIN=$1 P=$2 TOP=${3:-40}
total=$(grep -m1 '^# samples' $P | awk '{print $3}')
grep -v '^#' $P | grep -v ' lib ' | sort -k2 > /tmp/prof.$$.raw
# Samples in libraries are already named: count, "lib", library:symbol.
grep ' lib ' $P | awk '{print $1, $3}' > /tmp/prof.$$.lib
# A position-independent binary is named by offset, a fixed one by address.
col=2; readelf -h $BIN | grep -q 'EXEC (Executable' && col=3
awk -v c=$col '{printf "0x%s\n", $c}' /tmp/prof.$$.raw | addr2line -f -C -e $BIN | paste - - | awk '{print $1}' > /tmp/prof.$$.fn
{ paste -d' ' <(awk '{print $1}' /tmp/prof.$$.raw) /tmp/prof.$$.fn; cat /tmp/prof.$$.lib; } | \
  awk -v t=$total '{c[$2]+=$1} END {for (f in c) printf "%6.2f%% %7d %s\n", 100*c[f]/t, c[f], f}' | sort -rn | head -$TOP
rm -f /tmp/prof.$$.raw /tmp/prof.$$.fn /tmp/prof.$$.lib
