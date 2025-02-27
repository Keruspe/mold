#!/bin/bash
set -e
cd $(dirname $0)
mold=`pwd`/../../mold
echo -n "Testing $(basename -s .sh $0) ... "
t=$(pwd)/../../out/test/elf/$(basename -s .sh $0)
mkdir -p $t

ldd $mold-wrapper.so | grep -q libasan && { echo skipped; exit; }

cat <<'EOF' > $t/a.sh
#!/bin/bash
echo "$0" "$@"
EOF

chmod 755 $t/a.sh

cat <<'EOF' | cc -xc -o $t/exe -
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
  if (!strcmp(argv[1], "execl")) {
    execl("/usr/bin/ld", "/usr/bin/ld", "execl", (char *)0);
    perror("execl");
    return 1;
  }

  if (!strcmp(argv[1], "execlp")) {
    execlp("/usr/bin/ld", "/usr/bin/ld", "execlp", (char *)0);
    perror("execl");
    return 1;
  }

  if (!strcmp(argv[1], "execle")) {
    execle("/usr/bin/ld", "/usr/bin/ld", "execle", (char *)0, environ);
    perror("execl");
    return 1;
  }

  if (!strcmp(argv[1], "execv")) {
    execv("/usr/bin/ld", (char *[]){"/usr/bin/ld", "execv", (char *)0});
    perror("execl");
    return 1;
  }

  if (!strcmp(argv[1], "execvp")) {
    execvp("/usr/bin/ld", (char *[]){"/usr/bin/ld", "execvp", (char *)0});
    perror("execl");
    return 1;
  }

  if (!strcmp(argv[1], "execvpe")) {
    execvpe("/usr/bin/ld", (char *[]){"/usr/bin/ld", "execvpe", (char *)0},
            environ);
    perror("execl");
    return 1;
  }

  fprintf(stderr, "unreachable: %s\n", argv[1]);
  return 1;
}
EOF

LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execl | grep -q 'a.sh execl'
LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execlp | grep -q 'a.sh execlp'
LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execle | grep -q 'a.sh execle'
LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execv | grep -q 'a.sh execv'
LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execvp | grep -q 'a.sh execvp'
LD_PRELOAD=$mold-wrapper.so MOLD_PATH=$t/a.sh $t/exe execvpe | grep -q 'a.sh execvpe'

echo OK
