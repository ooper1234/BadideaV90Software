# V.90/V.92 current-source launcher fix — 2026-08-15

The old `START-V92-TEST.cmd` launched `windows/Start-V92ISP.ps1` directly without
building the edited source. `Start-V92ISP.ps1` then preferred historical staged
executables such as `v92isp-windows-cpt-ri-cancel-fixed.exe` over the canonical
`v92isp-windows.exe`.

That meant later CPt re-equalizer and TRN2d source fixes could be present in the
ZIP but never execute.

This package changes the test path to:

1. select V.92 mode;
2. run `START-V92ISP-WINDOWS.cmd`;
3. rebuild the current source through `Build-Windows.ps1`;
4. run only `dist\\v92isp-windows.exe`.

At startup verify these lines:

```
[START] Executable: ...\\dist\\v92isp-windows.exe
[START] Executable modified: <current build time>
[START] Historical staged *-fixed.exe binaries: ignored
```

During Phase 4, the new source also prints `adaptive CPt re-equalizer=enabled`
and prints CPt parameters (`S`, `K`, `ld`, `a1`, `a2`, `b1`, `b2`) when CPt
validates. If those strings do not appear, the current source build is not the
binary being executed.
