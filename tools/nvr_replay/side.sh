# Frame pairs side by side, one PNG: first folder, second folder, and their
# difference times 8 (alpha ignored: frames compare by colour).
#   bash tools/nvr_replay/side.sh DIR_A DIR_B OUT.png fNNN [fNNN...]
A=$1 B=$2 O=$3; shift 3
parts=()
for n in "$@"; do
  convert \( $A/$n.bmp -alpha off -resize 426x \) \( $B/$n.bmp -alpha off -resize 426x \) \
    \( \( $A/$n.bmp -alpha off \) \( $B/$n.bmp -alpha off \) -compose difference -composite \
       -evaluate multiply 8 -resize 426x \) +append /tmp/side_$$_$n.png
  parts+=(/tmp/side_$$_$n.png)
done
convert "${parts[@]}" -append $O
rm -f "${parts[@]}"
