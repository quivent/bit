#!/bin/sh
# Build every command twice: as a mappable object for the graft resident host,
# and as a standalone executable so the same source can be tested directly.
set -e
cd "$(dirname "$0")"
mkdir -p build/bin
CMDS=$(ls cmd/*.c | sed 's|cmd/||; s|\.c$||')
for c in $CMDS; do
  cc -O2 -fPIC -dynamiclib -o "build/$c.dylib" "cmd/$c.c" lib/bit.c -lz
  printf 'int cmd_main(int,char**);\nint main(int a,char**v){return cmd_main(a,v);}\n' > /tmp/bitmain.c
  cc -O2 -o "build/bin/$c" "cmd/$c.c" lib/bit.c /tmp/bitmain.c -lz
done
echo "built: $(echo $CMDS | tr ' ' ',')"
