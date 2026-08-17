# WinDivert / MinGW C++ header fix

The complete Windows build uses WinDivert as the preferred single-peer NAT path.
WinDivert 2.2.x defines the SAL annotation macros `__in`, `__out`, `__inout`, and
variants when compiling with MinGW. libstdc++ also uses identifiers such as
`__in` internally. If `windivert.h` is included before C++ standard-library
headers, those macros corrupt `<algorithm>`, `<istream>`, and `<ostream>` and
produce a large set of misleading template errors.

`src/windivert_nat.cpp` now:

1. includes all required C++ standard-library headers before `windivert.h`, and
2. undefines WinDivert's MinGW SAL macros immediately after the WinDivert API
   declarations have been parsed.

This fixes the Windows build failure at `src/windivert_nat.cpp.obj` without
changing the NAT protocol behavior.
