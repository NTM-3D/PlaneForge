# PlaneForge

PlaneForge reads, analyses and writes 3D-plane offset-metadata (`OFMD`) in raw MVC streams extracted from 3D Blu-rays, and interchanges that metadata with OFS 3D-Plane files.

## Building

With a C11 compiler on `PATH`:

```
build.bat                 # Windows convenience wrapper
```

or with `make`:

```
make
```

or manually:

```
windres src/version.rc -O coff -o src/version_rc.o
gcc -O2 -std=c11 -Wall -Wextra -o PlaneForge_v0.1.exe \
    src/annexb.c src/ofmd.c src/ofs.c src/mvc.c src/progress.c src/main.c \
    src/version_rc.o
```

On Windows the result is `PlaneForge_v0.1.exe`, carrying a `VERSIONINFO` resource; on POSIX it is `planeforge`.

## Usage

```
planeforge [options] <input> [output]
```

Run with no arguments to print the built-in help.

### Analysis

| Option | Description |
| --- | --- |
| `-analyze` | print an OFMD report as JSON |
| `-dump-sei` | dump the first OFMD SEI NAL |
| `-report FILE` | write the analysis report to `FILE` |
| `-show-empty` | include all-zero planes in the report |

If the input contains no OFMD, a per-NAL-type histogram is printed instead.

### Writing

These patch an existing stream in place when it already contains OFMD, or insert new OFMD SEI NALs at every GOP boundary when it does not. In the latter case `-base-view <base.avc>` is required.

| Option | Description |
| --- | --- |
| `-base-view FILE` | base-view AVC elementary stream (GOP boundaries) |
| `-plane N` | plane index `0..31` |
| `-depth N` | fixed depth `-127..127` |
| `-min-depth N` `-max-depth N` `-interval N` | sweeping depth; `-interval 1` updates every frame |
| `-batch-job FILE` | JSON batch job (array of entries) |
| `-force` | overwrite the output file if it exists |

```
planeforge in.mvc out.mvc -plane 0 -depth 5 -base-view base.avc
planeforge in.mvc out.mvc -plane 2 -min-depth -30 -max-depth 30 -interval 24
planeforge in.mvc out.mvc -batch-job job.json
```

Batch job format:

```json
[
  { "type": "static", "plane": 0, "depth": 5 },
  { "type": "sweep",  "plane": 1, "min-depth": -30, "max-depth": 30, "interval": 24 }
]
```

### OFS interchange

| Option | Description |
| --- | --- |
| `-export-ofs DIR` | export non-empty planes to `DIR/3D-Plane-NN.ofs` |
| `-plane N` | with export, write only plane `N` |
| `-fps N` | frame rate for export: `1,2,3,4,6,7` |
| `-dropframe` | set the drop-frame flag (frame rate 4 only) |
| `-from-ofs PATH` | import an OFS file or directory into the MVC output |
| `-plane SPEC` | with import, choose the destination plane(s) |
| `-override-depth N` | depth used to resolve frame-count mismatches |

When importing a single file without `-plane`, the plane id is taken from `guid[15]` inside the file. When importing a directory, `PATH/3D-Plane-NN.ofs` is loaded for each plane in ascending order.

The import `-plane` value accepts a single index (`5`), an inclusive range (`0-3`), a comma-separated list (`0,2,7`), or a combination (`0-3,7`). With no `-plane`, each file keeps the plane id from its GUID; with one target, files are assigned sequentially from that plane; with several targets, the count must equal the number of loaded files and they are assigned in order.

```
planeforge in.mvc -export-ofs planes/ -fps 4 -dropframe
planeforge in.mvc -export-ofs planes/ -plane 4
planeforge in.mvc out.mvc -from-ofs planes/
planeforge in.mvc out.mvc -from-ofs 3D-Plane-00.ofs -plane 12
planeforge in.mvc out.mvc -from-ofs planes/ -plane 0-10
```

### Progress

A progress line is printed while analyzing, writing or exporting:

```
write out.mvc:  63% [#####-------]  9.1 GiB / 14.4 GiB  142 MiB/s
```

It is written to standard error and can be turned off with `-quiet`.

## Frame-count mismatch behaviour

A plane is written only when its frame count matches the MVC stream.

* Without `-override-depth`, a mismatch is reported and PlaneForge exits with an error.
* With `-override-depth D`:
  * MVC longer than OFS: the OFS depth data is written first and the remaining frames are filled with depth `D`.
  * MVC shorter than OFS: every frame of the plane is written as depth `D`.

## Format notes

* OFS 3D-Plane file: 41-byte header (signature `89 4F 46 53 0D 0A 1A 0A`, version `0100`, 16-byte GUID at offset 12, frame-rate and drop-frame nibbles at offset 28, frame count at offset 37), then a little-endian `u64` length and one signed-magnitude depth byte per frame. Depth encoding: `0x80` = 0, `<0x80` = positive, `>0x80` = negative.
* OFMD block: `"OFMD"` magic, frame-rate byte, plane and frame counts, then `planes * frames` depth bytes, wrapped in the nested SEI container used on Blu-ray. When the plane count grows, the nested and nesting SEI payload sizes are rewritten.
* Base view: parsed access-unit by access-unit (the first slice of each picture has `first_mb_in_slice == 0`), so both single-slice and multi-slice encodings are supported. GOP boundaries are the IDR pictures; each GOP becomes one OFMD message, split if it exceeds the 127-frame OFMD limit.

## Source layout

| File | Contents |
| --- | --- |
| `src/pf.h` | shared types and declarations |
| `src/annexb.c` | Annex-B reader, RBSP/EBSP conversion |
| `src/ofmd.c` | OFMD parsing/construction and SEI wrapping |
| `src/ofs.c` | OFS file reader/writer |
| `src/mvc.c` | scan, writers, plane sources, export/import |
| `src/progress.c` | progress reporting |
| `src/main.c` | command-line interface and JSON batch parsing |

## Credits

OFMD/OFS format details follow [OFSExtractor](https://github.com/TheGreatMcPain/OFSExtractor).
