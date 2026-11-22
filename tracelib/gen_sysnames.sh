#!/bin/sh
here=$(cd "$(dirname "$0")" && pwd)
hdr="$here/../libc/include/purec/abi/syscall.h"
awk '/^#define SYS_[A-Z0-9_]+ [0-9]+$/ { n=$3+0; if (!(n in seen)) { seen[n]=1; name=tolower(substr($2,5)); print n, name } }' "$hdr" | sort -n |
awk 'BEGIN{print "static const struct tl_sysname tl_sysnames[] = {"} {printf "    {%d, \"%s\"},\n", $1, $2} END{print "};"}' > "$here/sysnames_table.inc"
