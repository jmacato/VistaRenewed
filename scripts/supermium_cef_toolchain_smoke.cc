#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <cstdio>

int main() {
  const std::vector<int> values{19, 23};
  std::printf("MSVC SDK smoke: %d; pid=%lu\n", values[0] + values[1],
              static_cast<unsigned long>(GetCurrentProcessId()));
  return values[0] + values[1] == 42 ? 0 : 1;
}
