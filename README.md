# rastertotmtr (universal)

Drop-in replacement for the Intel-only CUPS filter shipped in Epson's
`TMPrinter.pkg` (`/Library/Printers/EPSON/tmprinter/filter/rastertotmtr.app`).
It is a behaviour-compatible reimplementation in C, built as a universal
arm64 + x86_64 binary, so it keeps working once Rosetta 2 is gone.

The output byte stream is identical to the original filter for every
combination of the PPD options (verified by diffing against the original
running under Rosetta; see "Testing").

## What the filter does

CUPS hands it 1-bit raster pages; it emits ESC/POS:

| Stage | Bytes |
|---|---|
| Job start | `ESC = 1`, `ESC @`, `ESC c 0 2`, `ESC c 1 2`, `ESC c 3 0`, `GS P h v` |
| Drawer (OpenDrawer1/2) | `ESC p m 50 200` |
| Buzzer (Internal / External) | `ESC p 1 50 200` / `ESC ( A 5 0 97 100 1 50 200` |
| User files | `/Library/Caches/Epson/TerminalPrinter/<queue>_{StartJob,StartPage,EndPage,EndJob}.prn` copied verbatim if present |
| Each page | blank top/bottom rows trimmed per *TmxPaperReduction*, fed with `ESC J`, image sent in 256-line bands with `GS 8 L` (fn 112) + `GS ( L` (fn 50) |
| Cut | `ESC J 0` + `GS V B 0` per page or per job per *TmxPaperCut* |

Motion units come from `*TmxMotionUnitHori` / `*TmxMotionUnitVert` in the PPD.
Error codes printed as `ERROR: Error Code=N` match the original.

## Build

Requires Xcode command line tools (the macOS SDK still ships the CUPS headers
and link stubs).

```bash
make
```

produces `rastertotmtr.app` (ad-hoc signed universal binary).

## Install

```bash
sudo ./install.sh
```

Backs up the original bundle to `rastertotmtr.app.x86_64.bak` in the same
directory and copies the new bundle in place. The PPD's `*cupsFilter:` line
already points at this path, so existing EPSON TM queues use it on the next
job; no queue changes needed. `sudo ./install.sh --uninstall` restores the
backup.

## Testing

```bash
cd test
export PPD=/etc/cups/ppd/EPSON_TM_m30II.ppd
cupsfilter -p "$PPD" -m application/vnd.cups-raster receipt.txt > page.ras
../rastertotmtr 1 user title 1 "TmxPaperCut=CutPerJob" page.ras > out.bin
```

Compare `out.bin` against the original filter's output on the same raster
file (`cmp`). The two differ only if you invoke them with different
`argv[0]`, which changes the queue name in the debug output and the user-file
lookup path.

## Notes

* Depends on `libcups.2.dylib` and `libcupsimage.2.dylib`, both still part of
  macOS and used by Apple's own filters in `/usr/libexec/cups/filter`.
* One intentional divergence from a literal port: after the last page CUPS
  zeroes the page header, so the original's job-end feed computed `n / 0`.
  On Intel that silently truncated to "feed 0 lines"; on arm64 the same
  float-to-int conversion saturates and would feed metres of paper. The port
  makes the zero-resolution case an explicit no-op, matching the Intel output.
