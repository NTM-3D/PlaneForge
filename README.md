# PlaneForge

PlaneForge reads, analyses and writes 3D-plane offset-metadata (`OFMD`) in raw MVC streams extracted from 3D Blu-rays, and interchanges that metadata with OFS 3D-Plane files and with plane JSON files.

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
gcc -O2 -std=c11 -Wall -Wextra -o PlaneForge_v0.2.exe \
    src/annexb.c src/ofmd.c src/ofs.c src/mvc.c src/progress.c src/jval.c \
    src/pjson.c src/main.c src/version_rc.o
```

On Windows the result is `PlaneForge_v0.2.exe`, carrying a `VERSIONINFO` resource from `src/version.rc`; on POSIX it is `planeforge`. `build.bat` needs `gcc` and `windres` on `PATH` (for example from w64devkit or MinGW-w64), or the `CC` and `RC` environment variables pointing to them.

The version is set only in `src/version.h`; the program, the Windows resource, the exe name and both build scripts read it from there.

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

These patch an existing stream in place when it already contains OFMD, or insert new OFMD SEI NALs at every GOP boundary when it does not. In the latter case `-base-view <base.avc>` is required. The frame boundaries of the input are taken from its type-24 NALs, or from its AUD NALs when it holds none.

| Option | Description |
| --- | --- |
| `-base-view FILE` | base-view AVC elementary stream (GOP boundaries) |
| `-plane N` | plane index `0..31` |
| `-depth N` | fixed depth `-127..127`; depth `0` is not valid |
| `-min-depth N` `-max-depth N` `-interval N` | sweeping depth; `-interval 1` updates every frame |
| `-batch-job FILE` | JSON batch job (array of entries) |
| `-force` | overwrite the output file if it exists; also allow depth `0` |

Depth `0` marks an empty plane and is not a valid 3D-plane depth. Using it is reported as an error; with `-force` the affected frames are written as depth `+1` instead. The same rule applies to `-min-depth`/`-max-depth` and to the depths of a batch job.

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

### Export

Both formats are written with the same options; only the destination differs.

| Option | OFS | JSON |
| --- | --- | --- |
| `-export-ofs DIR` | `DIR/3D-Plane-NN.ofs` | |
| `-export-json PATH` | | a `.json` file holds every plane, a directory holds one `3D-Plane-NN.json` per plane |
| `-plane N` | export only plane `N` | export only plane `N` |
| `-fps N` | frame rate `1,2,3,4,6,7` | frame rate `1,2,3,4,6,7` |
| `-dropframe` | set the drop-frame flag (frame rate 4 only) | set the drop-frame flag (frame rate 4 only) |

Planes without depth data are left out of the export.

```
planeforge in.mvc -export-ofs planes/ -fps 4 -dropframe
planeforge in.mvc -export-ofs planes/ -plane 4
planeforge in.mvc -export-json planes.json
planeforge in.mvc -export-json planes/
```

### Import

Both formats are read with the same options; only the source differs.

| Option | OFS | JSON |
| --- | --- | --- |
| `-from-ofs PATH` | an OFS file or a directory of them | |
| `-from-json PATH` | | a `.json` file or a directory of them |
| `-plane SPEC` | choose the target plane(s) | choose the target plane(s) |
| `-override-depth N` | resolve frame-count mismatches | resolve frame-count mismatches |

When importing an OFS file without `-plane`, the plane id is taken from `guid[15]` inside the file. When importing a directory, `PATH/3D-Plane-NN.ofs` is loaded for each plane in ascending order; the same names are used for the JSON files of a directory.

The depth-0 rule of the writing options applies to imported planes as well: a plane that holds depth-0 frames is refused unless `-force` is given, and then those frames are written as depth `+1`.

The import `-plane` value accepts a single index (`5`), an inclusive range (`0-3`), a comma-separated list (`0,2,7`), or a combination (`0-3,7`). With no `-plane`, each file keeps the plane id from its GUID; with one target, files are assigned sequentially from that plane; with several targets, the count must equal the number of loaded files and they are assigned in order.

```
planeforge in.mvc out.mvc -from-ofs planes/
planeforge in.mvc out.mvc -from-ofs 3D-Plane-00.ofs -plane 12
planeforge in.mvc out.mvc -from-ofs planes/ -plane 0-10
planeforge in.mvc out.mvc -from-json planes.json
planeforge in.mvc out.mvc -from-json planes/ -plane 0-10
```

### Conversion

Both formats are converted into each other, with the input path as the source and the output path as the destination.

| Option | Description |
| --- | --- |
| `-ofs-to-json` | convert the input OFS file or directory to plane JSON |
| `-json-to-ofs` | convert the input plane JSON to an OFS file or directory |

```
planeforge planes/ -ofs-to-json planes.json
planeforge planes.json -json-to-ofs planes/
```

The conversion is lossless: the OFS and the JSON form of a plane hold the same data.

### File formats

Plane JSON holds the same information as an OFS file, with the per-frame depths run-length encoded into segments. A path ending in `.json` names one file that holds every plane; any other path names a directory that holds one `3D-Plane-NN.json` file per plane.

```json
{
  "format": "planeforge-planes",
  "version": 1,
  "frame_rate": 4,
  "drop_frame": false,
  "num_frames": 167955,
  "planes_details": {
    "4": {
      "plane_id": 4,
      "guid": "8f3c1a2b4d5e6f708192a3b4c5d6e7f8",
      "start_timecode": 0,
      "reserved": "00000000",
      "frame_rate": 4,
      "drop_frame": false,
      "num_frames": 167955,
      "segments": [
        { "start_frame": 0, "end_frame": 100, "depth": 1 },
        { "start_frame": 101, "end_frame": 167954, "depth": -7 }
      ]
    }
  }
}
```

The segments of a plane must cover its frames without a gap, and `end_frame` is inclusive. A plane is named by its plane id, which is also the last byte of its GUID. A file that holds no GUID gets a fresh one, and the GUID of a plane is not part of an OFMD block, so it is lost when metadata is written into an MVC stream and regenerated on the next export.

The frame rate and drop-frame flag of the file are the defaults of its planes; a plane may carry its own values, which is what keeps the conversion lossless when files of different frame rates are combined.

### Progress

A progress line is printed while analyzing, writing or exporting:

```
write out.mvc:  63% [#####-------]  9.1 GiB / 14.4 GiB  142 MiB/s
```

It is written to standard error and can be turned off with `-quiet`. `-h`/`-help` prints the built-in help and `-V`/`-version` prints the version.

## Test suite

`test_suite.py` runs every test scenario against one input file and reports the result of each. The input type is detected automatically:

* An MVC stream that already contains offset-metadata: the export, conversion, import and depth-0 scenarios run.
* An MVC stream without offset-metadata: the scenarios that create planes from scratch run. A base view is required; the input is used as its own base view when it contains one, otherwise one is looked for next to it.

```
python test_suite.py <input>
```

Before it starts, the suite shows the disk space it needs and asks for confirmation. Each scenario prints one line with its status, duration and result. A text report, `<input>_report.txt`, is written next to the input, and the test outputs go to `<input>_test-outputs` by default.

| Argument | Description |
| --- | --- |
| `--exe PATH` | binary to test (default: the newest `PlaneForge_*.exe` or `planeforge` next to the script) |
| `--results DIR` | folder for the test outputs (default: `<input>_test-outputs` next to the input) |
| `--cleanup` | delete test outputs as the run proceeds, to save disk space |
| `--fast` | skip scenarios that write a full copy of the stream |
| `--json` | also write `<input>_report.json` |
| `--force` | start without asking for confirmation |

## Frame-count mismatch behaviour

A plane is written only when its frame count matches the MVC stream.

* Without `-override-depth`, a mismatch is reported and PlaneForge exits with an error.
* With `-override-depth D`:
  * MVC longer than the plane data: the plane depth data is written first and the remaining frames are filled with depth `D`.
  * MVC shorter than the plane data: every frame of the plane is written as depth `D`.

## Format notes

* OFS 3D-Plane file: 41-byte header (signature `89 4F 46 53 0D 0A 1A 0A`, version `0100`, 16-byte GUID at offset 12, frame-rate and drop-frame nibbles at offset 28, four reserved bytes at offset 29, start timecode at offset 33, frame count at offset 37), then a little-endian `u64` length and one signed-magnitude depth byte per frame. Depth encoding: `0x80` = 0, `<0x80` = positive, `>0x80` = negative.
* Plane JSON: the same fields as an OFS file, with the depth bytes replaced by run-length encoded segments and the GUID kept as 32 hex digits.
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
| `src/jval.c` | JSON value parser |
| `src/pjson.c` | plane JSON reader/writer |
| `src/main.c` | command-line interface and JSON batch parsing |
| `src/version.h` | the version number (single source) |
| `src/version.rc` | Windows `VERSIONINFO` resource |
| `test_suite.py` | end-to-end test suite |

## Credits

OFMD/OFS format details follow [OFSExtractor](https://github.com/TheGreatMcPain/OFSExtractor).
