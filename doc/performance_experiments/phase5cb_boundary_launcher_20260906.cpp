// Fixed ELF entry points for the existing paired runner. No timing/statistics
// here: exec the same Python benchmark with a compile-time repo identity.
#include <unistd.h>
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
  std::vector<char *> args{
      const_cast<char *>(CSPLENDOR_PYTHON_BINARY),
      const_cast<char *>(CSPLENDOR_BOUNDARY_SCRIPT),
      const_cast<char *>("--repo"), const_cast<char *>(CSPLENDOR_BOUNDARY_REPO)};
#ifdef CSPLENDOR_BOUNDARY_LEGACY
  args.push_back(const_cast<char *>("--legacy"));
#endif
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  args.push_back(nullptr);
  execv(CSPLENDOR_PYTHON_BINARY, args.data());
  std::perror("exec Python boundary benchmark");
  return 127;
}
