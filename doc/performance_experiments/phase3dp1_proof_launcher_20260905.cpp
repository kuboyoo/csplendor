// ELF command adapter for the existing paired runner, not a timing framework.
// Only launcher inodes rotate; Python and extension inodes do not rotate.
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  execl(PYTHON_PATH, PYTHON_PATH, PROBE_PATH, "--package-root", PACKAGE_ROOT,
        argv[1], argv[2], static_cast<char *>(nullptr));
  return 127;
}
