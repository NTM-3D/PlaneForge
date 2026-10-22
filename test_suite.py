#!/usr/bin/env python3
"""PlaneForge test suite.

Runs every scenario PlaneForge supports against one input file and checks the
result of each one.  The kind of input is detected from the file itself:

  * an MVC stream that already carries 3D-plane offset-metadata, and
  * an MVC stream without offset-metadata, in which case the planes are
    created from scratch; when the file holds a base view of its own the file
    is used as the base view, otherwise a base view is looked for next to it.

    python test_suite.py <input>
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

OFS_SIGNATURE = bytes.fromhex("894f46530d0a1a0a")
OFS_HEADER = 41          # fixed header, the frame count follows it
OFS_DATA = 49            # first depth byte
MAX_PLANES = 32

TOOL_PATTERNS = ("PlaneForge_*.exe", "PlaneForge.exe", "planeforge")


def find_tool():
    """The newest PlaneForge binary next to the script or in the working folder."""
    for folder in dict.fromkeys((os.path.dirname(os.path.abspath(__file__)), os.getcwd())):
        for pattern in TOOL_PATTERNS:
            found = [f for f in glob.glob(os.path.join(folder, pattern)) if os.path.isfile(f)]
            if found:
                return max(found, key=os.path.getmtime)
    return None


class Fail(Exception):
    """A check did not hold."""


class Skip(Exception):
    """A scenario cannot run with this input."""


# ── helpers ──────────────────────────────────────────────────────────────
def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def count(n, noun):
    """A number and a noun, pluralised and grouped."""
    return "{:,} {}{}".format(n, noun, "" if n == 1 else "s")


def read_ofs(path):
    """Parse an OFS 3D-Plane file into a dictionary."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] != OFS_SIGNATURE:
        raise Fail("%s is not an OFS file." % os.path.basename(path))
    guid = raw[12:28]
    plane_id = guid[15]
    frame_rate = (raw[28] >> 4) & 0x0f
    drop_frame = raw[28] & 0x0f
    reserved = raw[29:33]
    start_timecode = struct.unpack_from("<I", raw, 33)[0]
    num_frames = struct.unpack_from("<I", raw, 37)[0]
    length = struct.unpack_from("<Q", raw, 41)[0]
    if length != num_frames:
        raise Fail("%s holds a length of %d, expected %d."
                   % (os.path.basename(path), length, num_frames))
    data = raw[OFS_DATA:]
    if len(data) != num_frames:
        raise Fail("%s holds %s, expected %s."
                   % (os.path.basename(path), count(len(data), "depth byte"),
                      count(num_frames, "depth byte")))
    depths = [-(b & 0x7f) if b & 0x80 else (b & 0x7f) for b in data]
    return {
        "path": path,
        "name": os.path.basename(path),
        "plane_id": plane_id,
        "guid": guid.hex(),
        "frame_rate": frame_rate,
        "drop_frame": bool(drop_frame),
        "reserved": reserved.hex(),
        "start_timecode": start_timecode,
        "num_frames": num_frames,
        "depths": depths,
    }


def read_plane_json(path):
    """Parse a plane JSON file into a dictionary."""
    with open(path, "r", encoding="utf-8-sig") as f:
        doc = json.load(f)
    if doc.get("format") != "planeforge-planes":
        raise Fail("%s is not a plane JSON file." % os.path.basename(path))
    planes = {}
    for key, plane in doc.get("planes_details", {}).items():
        depths = []
        for seg in plane.get("segments", []):
            start = seg["start_frame"]
            end = seg["end_frame"]
            if end < start:
                raise Fail("%s: plane %s has a segment that ends before it starts."
                           % (os.path.basename(path), key))
            depths.extend([seg["depth"]] * (end - start + 1))
        planes[int(key)] = {
            "plane_id": plane.get("plane_id", int(key)),
            "guid": plane.get("guid", ""),
            "frame_rate": plane.get("frame_rate", doc.get("frame_rate")),
            "drop_frame": plane.get("drop_frame", doc.get("drop_frame")),
            "num_frames": plane.get("num_frames", doc.get("num_frames")),
            "depths": depths,
            "segments": plane.get("segments", []),
        }
    return {
        "path": path,
        "name": os.path.basename(path),
        "frame_rate": doc.get("frame_rate"),
        "drop_frame": doc.get("drop_frame"),
        "num_frames": doc.get("num_frames"),
        "planes": planes,
    }


def segments_of(depths):
    """Run-length encode a depth sequence into JSON segments."""
    segments = []
    for i, d in enumerate(depths):
        if segments and segments[-1]["depth"] == d:
            segments[-1]["end_frame"] = i
            continue
        segments.append({"start_frame": i, "end_frame": i, "depth": d})
    return segments


def plane_json_doc(plane_id, depths, frame_rate=1, drop_frame=False):
    """Build a plane JSON document that holds a single plane."""
    return {
        "format": "planeforge-planes",
        "version": 1,
        "frame_rate": frame_rate,
        "drop_frame": drop_frame,
        "num_frames": len(depths),
        "planes_details": {
            str(plane_id): {
                "plane_id": plane_id,
                "guid": "%032x" % (0x5a5a0000 + plane_id),
                "start_timecode": 0,
                "reserved": "00000000",
                "frame_rate": frame_rate,
                "drop_frame": drop_frame,
                "num_frames": len(depths),
                "segments": segments_of(depths),
            }
        },
    }


def depth_stats(depths):
    """The plane statistics the analyser reports for a depth sequence."""
    if not depths:
        return None
    lo, hi = min(depths), max(depths)
    changes = sum(1 for i in range(1, len(depths)) if depths[i] != depths[i - 1])
    kind = "empty" if lo == 0 and hi == 0 else ("static" if lo == hi else "dynamic")
    return {"type": kind, "frames": len(depths), "min": lo, "max": hi, "changes": changes}


def sweep_values(lo, hi, interval, frames):
    """The depths a sweep writes, frame by frame."""
    if lo > hi:
        lo, hi = hi, lo
    if lo == hi:
        return [lo] * frames
    cur = 0 if lo < 0 else lo
    if cur > hi:
        cur = hi
    if cur == 0:
        cur = 1
    direction = 1
    count = 0
    out = []
    for _ in range(frames):
        d = cur
        count += 1
        if count >= interval:
            count = 0
            if direction == 1:
                if cur >= hi:
                    direction = -1
                    cur -= 1
                else:
                    cur += 1
                if cur == 0:
                    cur = 1
            else:
                if cur <= lo:
                    direction = 1
                    cur += 1
                else:
                    cur -= 1
                if cur == 0:
                    cur = -1
        out.append(d)
    return out


def parse_nal_histogram(text):
    """Read the NAL histogram the analyser prints for a stream without metadata."""
    counts = {}
    for m in re.finditer(r"type\s+(\d+)\s+\(.*\):\s+(\d+)", text):
        counts[int(m.group(1))] = int(m.group(2))
    return counts


def find_base_view(path):
    """Look for a base view next to an MVC stream that has none of its own."""
    stem = os.path.splitext(path)[0]
    for ext in (".avc", ".264", ".h264", ".base"):
        candidate = stem + ext
        if os.path.isfile(candidate):
            return candidate
    folder = os.path.dirname(os.path.abspath(path))
    base = os.path.basename(stem)
    for name in sorted(os.listdir(folder)):
        if name.lower().endswith((".avc", ".264", ".h264")) and base.lower() in name.lower():
            return os.path.join(folder, name)
    return None


def write_ofs(path, plane_id, depths, frame_rate=1, drop_frame=False, guid=None):
    """Write an OFS 3D-Plane file."""
    body = bytes((0x80 if d == 0 else (0x80 - d if d < 0 else d)) & 0xff for d in depths)
    if guid is None:
        guid = bytes(range(16))
    guid = bytearray(guid)
    guid[15] = plane_id
    header = bytearray(OFS_DATA)
    header[0:8] = OFS_SIGNATURE
    header[8:12] = b"0100"
    header[12:28] = guid
    header[28] = ((frame_rate & 0x0f) << 4) | (1 if drop_frame else 0)
    struct.pack_into("<I", header, 37, len(depths))
    struct.pack_into("<Q", header, 41, len(depths))
    folder = os.path.dirname(path)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(path, "wb") as f:
        f.write(bytes(header) + body)


def human(size):
    """A size in bytes as a readable string."""
    value = float(size)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024 or unit == "TB":
            return "%.1f %s" % (value, unit)
        value /= 1024
    return "%.1f TB" % value


# ── the suite ────────────────────────────────────────────────────────────
class Suite:
    def __init__(self, exe, input_path, results, cleanup=False):
        self.exe = exe
        self.input = os.path.abspath(input_path)
        self.results = os.path.abspath(results)
        self.cleanup = cleanup
        self.rows = []
        self.notes = []
        self.kind = "unknown"
        self.base_view = None
        self.last_text = ""
        self.planes = {}          # plane id -> OFS plane data
        self.frame_count = 0
        self.has_depth_zero = False
        self.index = {}           # scenario name -> number

    # ── running the tool ──
    def tool(self, args, expect_ok=True):
        cmd = [self.exe] + [str(a) for a in args]
        proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if expect_ok and proc.returncode != 0:
            tail = (proc.stderr or proc.stdout).strip().splitlines()
            raise Fail("Exit %d: %s\n        %s"
                       % (proc.returncode, " ".join(str(a) for a in args),
                          tail[-1] if tail else ""))
        return proc

    # ── outputs ──
    def base(self, key, suffix=""):
        """Name of an output, numbered after the order the scenarios run in."""
        return "%02d_%s%s" % (self.index[ARTEFACTS[key]], key, suffix)

    def out(self, name, *parts):
        """Path of an output in the results folder."""
        path = os.path.join(self.results, name, *parts)
        folder = os.path.dirname(path)
        if folder:
            os.makedirs(folder, exist_ok=True)
        return path

    def folder(self, name):
        """Path of a folder in the results."""
        path = os.path.join(self.results, name)
        os.makedirs(path, exist_ok=True)
        return path

    def base_args(self):
        """The base view the writer needs for a stream without metadata."""
        if self.kind != "no-planes":
            return []
        if not self.base_view:
            raise Skip("No base view was found for the input.")
        return ["-base-view", self.base_view]

    # ── reporting ──
    def record(self, name, status, detail, artifacts=()):
        self.rows.append({"name": name, "status": status, "detail": detail,
                          "description": SCENARIOS[name][1],
                          "artifacts": [os.path.relpath(a, self.results) for a in artifacts]})

    def scenario(self, name):
        try:
            detail, artifacts = SCENARIOS[name][0](self)
        except Skip as exc:
            self.record(name, "SKIP", str(exc))
            return False
        except Fail as exc:
            self.record(name, "FAIL", str(exc))
            return False
        except OSError as exc:
            self.record(name, "FAIL", "%s" % exc)
            return False
        self.record(name, "PASS", detail, artifacts)
        return True

    # ── analysis ──
    def analyze(self, path, save=None):
        """Run the analyser; the report is None for a stream without metadata."""
        proc = self.tool(["-quiet", "-analyze", "-show-empty", path])
        self.last_text = proc.stdout.strip()
        if save:
            with open(save, "w", encoding="utf-8") as f:
                f.write(proc.stdout)
        if self.last_text.startswith("{"):
            return json.loads(self.last_text)
        return None

    def report(self, path):
        report = self.analyze(path)
        if report is None:
            raise Fail("%s holds no offset-metadata." % os.path.basename(path))
        return report

    # ── checks used by several scenarios ──
    def check_planes_against(self, report, planes, label):
        """The report must describe the planes the way the files do."""
        details = report.get("plane_details", {})
        for plane_id, data in sorted(planes.items()):
            want = depth_stats(data["depths"])
            got = details.get(str(plane_id))
            if got is None:
                raise Fail("%s: plane %d is missing from the report." % (label, plane_id))
            for key in ("type", "frames", "min", "max", "changes"):
                if got.get(key) != want[key]:
                    raise Fail("%s: plane %d reports %s %r, expected %r."
                               % (label, plane_id, key, got.get(key), want[key]))

    def check_report_planes_empty(self, report, used, label):
        for plane_id, data in report.get("plane_details", {}).items():
            if int(plane_id) in used:
                continue
            if data.get("type") != "empty":
                raise Fail("%s: plane %s should be empty but is %r."
                           % (label, plane_id, data.get("type")))

    # ── removing outputs ──
    def remove(self, path):
        root = os.path.abspath(self.results)
        if not os.path.abspath(path).startswith(root + os.sep):
            return
        try:
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
        except OSError:
            return
        root = os.path.abspath(self.results)
        folder = os.path.dirname(os.path.abspath(path))
        while folder != root and folder.startswith(root + os.sep):
            try:
                os.rmdir(folder)
            except OSError:
                return
            folder = os.path.dirname(folder)

    def sweep(self, order, index):
        for name in order[:index + 1]:
            row = self.row_of(name)
            if row is None:
                continue
            pending = [c for c in SCENARIOS[name][3]
                       if c in order and order.index(c) > index]
            if pending:
                continue
            for rel in row["artifacts"]:
                self.remove(os.path.join(self.results, rel))
        self.remove_empty_folders(self.results)

    def remove_empty_folders(self, root):
        root = os.path.abspath(root)
        if not os.path.isdir(root):
            return
        for dirpath, dirnames, filenames in os.walk(root, topdown=False):
            if dirpath == root:
                continue
            if not dirnames and not filenames:
                try:
                    os.rmdir(dirpath)
                except OSError:
                    pass

    def row_of(self, name):
        for row in self.rows:
            if row["name"] == name:
                return row
        return None


def plane_stat(report, plane_id, label):
    got = report.get("plane_details", {}).get(str(plane_id))
    if got is None:
        raise Fail("%s: plane %d is missing from the report." % (label, plane_id))
    return got


# ── scenarios ────────────────────────────────────────────────────────────
def sc_analyze(s):
    path = s.out(s.base("analyze", ".json" if s.kind == "planes" else ".txt"))
    report = s.analyze(s.input, save=path)
    if s.kind == "planes":
        if report is None:
            raise Fail("The analyser did not find any offset-metadata.")
        if report.get("ofmd_messages", 0) <= 0:
            raise Fail("The input should hold offset-metadata.")
        if report.get("planes", 0) <= 0:
            raise Fail("The input should hold planes.")
        frames = [p["frames"] for p in report.get("plane_details", {}).values()]
        s.frame_count = max(frames) if frames else 0
        detail = "%s, %s, %s." % (count(report["ofmd_messages"], "OFMD message"),
                                  count(report["planes"], "plane"),
                                  count(s.frame_count, "frame"))
    else:
        if report is not None:
            raise Fail("The input should not hold offset-metadata.")
        counts = parse_nal_histogram(s.last_text)
        s.notes.append("The stream holds %s and %s."
                       % (count(counts.get(24, 0), "type-24 frame marker"),
                          count(counts.get(9, 0), "AUD")))
        detail = "No offset metadata; %s." % count(sum(counts.values()), "NAL unit")
    return detail, [path]


def sc_export_ofs(s):
    folder = s.out(s.base("export_ofs"))
    proc = s.tool(["-force", "-quiet", "-export-ofs", folder, s.input])
    files = sorted(f for f in os.listdir(folder) if f.endswith(".ofs"))
    if not files:
        raise Fail("No OFS file was written.")
    planes = {}
    for name in files:
        data = read_ofs(os.path.join(folder, name))
        if name != "3D-Plane-%02d.ofs" % data["plane_id"]:
            raise Fail("%s holds plane %d." % (name, data["plane_id"]))
        if data["num_frames"] == 0:
            raise Fail("%s holds no frame." % name)
        planes[data["plane_id"]] = data
    s.planes = planes
    s.has_depth_zero = "depth-0" in proc.stderr
    if s.has_depth_zero:
        s.notes.append("The stream holds depth-0 frames; they were written as +1.")
    return "%s exported, %s each." % (
        count(len(planes), "plane"),
        count(max(p["num_frames"] for p in planes.values()), "frame")), \
        [os.path.join(folder, f) for f in files]


def sc_export_no_force(s):
    folder = s.out(s.base("export_ofs_no_force"))
    proc = s.tool(["-quiet", "-export-ofs", folder, s.input], expect_ok=False)
    if s.has_depth_zero:
        if proc.returncode == 0:
            raise Fail("The depth-0 frames were accepted without -force.")
        if "depth 0" not in proc.stderr:
            raise Fail("Depth 0 was not reported.")
        return "Depth-0 frames rejected without -force.", []
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout).strip().splitlines()
        raise Fail("The export failed: %s" % (tail[-1] if tail else ""))
    files = sorted(f for f in os.listdir(folder) if f.endswith(".ofs"))
    if not files:
        raise Fail("No OFS file was written.")
    return "%s exported without -force." % count(len(files), "plane"), \
        [os.path.join(folder, f) for f in files]


def sc_export_json(s):
    path = s.out(s.base("export_json"), "planes.json")
    s.tool(["-force", "-quiet", "-export-json", path, s.input])
    doc = read_plane_json(path)
    if not doc["planes"]:
        raise Fail("The JSON file holds no plane.")
    for plane_id, plane in doc["planes"].items():
        if plane_id not in s.planes:
            raise Fail("Plane %d is not part of the OFS export." % plane_id)
        if plane["depths"] != s.planes[plane_id]["depths"]:
            raise Fail("Plane %d holds different depths than the OFS file." % plane_id)
        if plane["frame_rate"] != s.planes[plane_id]["frame_rate"]:
            raise Fail("Plane %d holds frame rate %r, expected %r."
                       % (plane_id, plane["frame_rate"], s.planes[plane_id]["frame_rate"]))
        if bool(plane["drop_frame"]) != s.planes[plane_id]["drop_frame"]:
            raise Fail("Plane %d holds a different drop-frame flag." % plane_id)
    return "%s in one file." % count(len(doc["planes"]), "plane"), [path]


def sc_export_json_dir(s):
    folder = s.out(s.base("export_json_dir"))
    s.tool(["-force", "-quiet", "-export-json", folder, s.input])
    files = sorted(f for f in os.listdir(folder) if f.endswith(".json"))
    if len(files) != len(s.planes):
        raise Fail("%s written, expected %s."
                   % (count(len(files), "file"), count(len(s.planes), "file")))
    for name in files:
        doc = read_plane_json(os.path.join(folder, name))
        if len(doc["planes"]) != 1:
            raise Fail("%s holds %s." % (name, count(len(doc["planes"]), "plane")))
        plane_id = list(doc["planes"])[0]
        if name != "3D-Plane-%02d.json" % plane_id:
            raise Fail("%s holds plane %d." % (name, plane_id))
        if doc["planes"][plane_id]["depths"] != s.planes[plane_id]["depths"]:
            raise Fail("%s holds different depths than the OFS file." % name)
    return "%s, one per plane." % count(len(files), "file"), [os.path.join(folder, f) for f in files]


def sc_ofs_to_json(s):
    path = s.out(s.base("ofs_to_json"), "planes.json")
    s.tool(["-force", "-quiet", "-ofs-to-json", s.out(s.base("export_ofs")), path])
    doc = read_plane_json(path)
    if len(doc["planes"]) != len(s.planes):
        raise Fail("%s converted, expected %s."
                   % (count(len(doc["planes"]), "plane"), count(len(s.planes), "plane")))
    for plane_id, plane in doc["planes"].items():
        if plane["depths"] != s.planes[plane_id]["depths"]:
            raise Fail("Plane %d holds different depths than the OFS file." % plane_id)
        if plane["guid"] != s.planes[plane_id]["guid"]:
            raise Fail("Plane %d holds a different GUID." % plane_id)
    return "%s converted." % count(len(doc["planes"]), "plane"), [path]


def sc_json_to_ofs(s):
    folder = s.out(s.base("json_to_ofs"))
    s.tool(["-force", "-quiet", "-json-to-ofs",
            s.out(s.base("ofs_to_json"), "planes.json"), folder])
    files = sorted(f for f in os.listdir(folder) if f.endswith(".ofs"))
    if len(files) != len(s.planes):
        raise Fail("%s written, expected %s."
                   % (count(len(files), "file"), count(len(s.planes), "file")))
    for name in files:
        a = sha256(os.path.join(s.out(s.base("export_ofs")), name))
        b = sha256(os.path.join(folder, name))
        if a != b:
            raise Fail("%s differs from the OFS export." % name)
    return "%s identical to the export." % count(len(files), "file"), \
        [os.path.join(folder, f) for f in files]


def sc_import_ofs(s):
    path = s.out(s.base("import_ofs", ".mvc"))
    s.tool(["-quiet", "-from-ofs", s.out(s.base("export_ofs")), s.input, path])
    report = s.report(path)
    s.check_planes_against(report, s.planes, "The imported stream")
    return "%s imported." % count(len(s.planes), "plane"), [path]


def sc_import_json(s):
    path = s.out(s.base("import_json", ".mvc"))
    s.tool(["-quiet", "-from-json", s.out(s.base("export_json"), "planes.json"),
            s.input, path])
    report = s.report(path)
    s.check_planes_against(report, s.planes, "The imported stream")
    same = sha256(path) == sha256(s.out(s.base("import_ofs", ".mvc")))
    if not same:
        raise Fail("The JSON import differs from the OFS import.")
    return "%s imported; matches the OFS import." % count(len(s.planes), "plane"), [path]


def sc_import_plane(s):
    plane_id = sorted(s.planes)[0]
    source = os.path.join(s.out(s.base("export_ofs")), "3D-Plane-%02d.ofs" % plane_id)
    path = s.out(s.base("import_plane", ".mvc"))
    s.tool(["-quiet", "-from-ofs", source, s.input, path, "-plane", str(plane_id)])
    report = s.report(path)
    s.check_planes_against(report, {plane_id: s.planes[plane_id]}, "The imported stream")
    return "Plane %d imported." % plane_id, [path]


def sc_import_list(s):
    ids = sorted(s.planes)[:2]
    if len(ids) < 2:
        raise Skip("The input holds fewer than two planes.")
    folder = s.folder(s.base("import_list", "_planes"))
    for i in ids:
        shutil.copyfile(os.path.join(s.out(s.base("export_ofs")), "3D-Plane-%02d.ofs" % i),
                        os.path.join(folder, "3D-Plane-%02d.ofs" % i))
    path = s.out(s.base("import_list", ".mvc"))
    s.tool(["-quiet", "-from-ofs", folder, s.input, path,
            "-plane", "%d-%d" % (ids[0], ids[1])])
    report = s.report(path)
    s.check_planes_against(report, {i: s.planes[i] for i in ids}, "The imported stream")
    return "Planes %d and %d imported." % (ids[0], ids[1]), [folder, path]


def sc_mismatch(s):
    plane_id = sorted(s.planes)[0]
    data = s.planes[plane_id]
    total = data["num_frames"]
    short = total - max(1, total // 4)
    if short <= 0:
        raise Skip("The plane holds too few frames.")
    doc = plane_json_doc(plane_id, data["depths"][:short],
                         s.planes[plane_id]["frame_rate"],
                         s.planes[plane_id]["drop_frame"])
    path = s.out(s.base("mismatch", ".json"))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)

    out = s.out(s.base("mismatch", ".mvc"))
    proc = s.tool(["-quiet", "-from-json", path, s.input, out], expect_ok=False)
    if proc.returncode == 0:
        raise Fail("A frame-count mismatch was not reported.")
    if "-override-depth" not in proc.stderr:
        raise Fail("The mismatch was not reported.")

    s.tool(["-force", "-quiet", "-from-json", path, s.input, out, "-override-depth", "7"])
    report = s.report(out)
    got = plane_stat(report, plane_id, "The imported stream")
    want = list(data["depths"][:short]) + [7] * (total - short)
    if got["min"] != min(want) or got["max"] != max(want):
        raise Fail("Plane %d holds depths %d..%d, expected %d..%d."
                   % (plane_id, got["min"], got["max"], min(want), max(want)))
    if got["frames"] != total:
        raise Fail("Plane %d holds %s, expected %s."
                   % (plane_id, count(got["frames"], "frame"), count(total, "frame")))
    return "Mismatch reported; -override-depth pads with depth 7.", [path, out]


def sc_depth_zero(s):
    plane_id = sorted(s.planes)[0]
    data = s.planes[plane_id]
    depths = list(data["depths"])
    for i in range(0, len(depths), 3):
        depths[i] = 0
    if all(d == 0 for d in depths):
        raise Skip("The plane holds no depth of its own.")
    doc = plane_json_doc(plane_id, depths)
    path = s.out(s.base("depth_zero", ".json"))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)

    out = s.out(s.base("depth_zero", ".mvc"))
    proc = s.tool(["-quiet", "-from-json", path, s.input, out], expect_ok=False)
    if proc.returncode == 0:
        raise Fail("A depth-0 plane was accepted.")
    if "depth 0" not in proc.stderr:
        raise Fail("Depth 0 was not reported.")

    s.tool(["-force", "-quiet", "-from-json", path, s.input, out])
    report = s.report(out)
    got = plane_stat(report, plane_id, "The imported stream")
    want = [1 if d == 0 else d for d in depths]
    if got["min"] != min(want) or got["max"] != max(want):
        raise Fail("Plane %d holds depths %d..%d, expected %d..%d."
                   % (plane_id, got["min"], got["max"], min(want), max(want)))
    return "Depth 0 rejected; -force writes depth 1.", [path, out]


def sc_depth_option(s):
    out = s.out(s.base("depth_option", ".mvc"))
    args = ["-quiet", "-plane", "0", "-depth", "0"] + s.base_args() + [s.input, out]
    proc = s.tool(args, expect_ok=False)
    if proc.returncode == 0:
        raise Fail("A depth of 0 was accepted.")
    if "depth 0" not in proc.stderr:
        raise Fail("Depth 0 was not reported.")
    s.tool(["-force"] + args[1:])
    report = s.report(out)
    got = plane_stat(report, 0, "The written stream")
    if got["min"] != 1 or got["max"] != 1:
        raise Fail("Plane 0 holds depths %d..%d, expected 1..1." % (got["min"], got["max"]))
    return "Depth 0 rejected; -force writes depth 1.", [out]


def sc_export_fps(s):
    folder = s.out(s.base("export_fps4"))
    s.tool(["-force", "-quiet", "-export-ofs", folder, s.input, "-fps", "4", "-dropframe"])
    files = sorted(f for f in os.listdir(folder) if f.endswith(".ofs"))
    if not files:
        raise Fail("No OFS file was written.")
    for name in files:
        data = read_ofs(os.path.join(folder, name))
        if data["frame_rate"] != 4:
            raise Fail("%s holds frame rate %d." % (name, data["frame_rate"]))
        if not data["drop_frame"]:
            raise Fail("%s is not marked as drop-frame." % name)
    return "%s at frame rate 4, drop-frame." % count(len(files), "plane"), \
        [os.path.join(folder, f) for f in files]


def sc_export_single_plane(s):
    plane_id = sorted(s.planes)[0]
    folder = s.out(s.base("export_single_plane"))
    s.tool(["-force", "-quiet", "-export-ofs", folder, s.input, "-plane", str(plane_id)])
    files = sorted(f for f in os.listdir(folder) if f.endswith(".ofs"))
    if len(files) != 1:
        raise Fail("%s written, expected 1." % count(len(files), "file"))
    data = read_ofs(os.path.join(folder, files[0]))
    if data["plane_id"] != plane_id:
        raise Fail("%s holds plane %d." % (files[0], data["plane_id"]))
    return "Only plane %d written." % plane_id, [os.path.join(folder, files[0])]


def sc_create_depth(s):
    out = s.out(s.base("create_depth", ".mvc"))
    s.tool(["-force", "-quiet", "-plane", "0", "-depth", "5", "-base-view", s.base_view,
            s.input, out])
    report = s.report(out)
    got = plane_stat(report, 0, "The created stream")
    if got["min"] != 5 or got["max"] != 5:
        raise Fail("Plane 0 holds depths %d..%d, expected 5..5." % (got["min"], got["max"]))
    s.check_report_planes_empty(report, {0}, "The created stream")
    s.frame_count = got["frames"]
    return "Plane 0 created at depth 5 across %s." % count(got["frames"], "frame"), [out]


def sc_create_sweep(s):
    out = s.out(s.base("create_sweep", ".mvc"))
    s.tool(["-force", "-quiet", "-plane", "1", "-min-depth", "-4", "-max-depth", "4",
            "-interval", "2", "-base-view", s.base_view, s.input, out])
    report = s.report(out)
    got = plane_stat(report, 1, "The created stream")
    want = depth_stats(sweep_values(-4, 4, 2, got["frames"]))
    for key in ("min", "max", "changes", "frames"):
        if got.get(key) != want[key]:
            raise Fail("Plane 1 reports %s %r, expected %r."
                       % (key, got.get(key), want[key]))
    s.check_report_planes_empty(report, {1}, "The created stream")
    return "Plane 1 sweeps from %d to %d across %s." % (
        got["min"], got["max"], count(got["frames"], "frame")), [out]


def sc_create_batch(s):
    job = s.out(s.base("create_batch", "_job.json"))
    with open(job, "w", encoding="utf-8") as f:
        json.dump([{"type": "static", "plane": 2, "depth": 9},
                   {"type": "sweep", "plane": 3, "min-depth": -2, "max-depth": 2,
                    "interval": 3}], f, indent=2)
    out = s.out(s.base("create_batch", ".mvc"))
    s.tool(["-force", "-quiet", "-batch-job", job, "-base-view", s.base_view, s.input, out])
    report = s.report(out)
    two = plane_stat(report, 2, "The created stream")
    three = plane_stat(report, 3, "The created stream")
    if two["min"] != 9 or two["max"] != 9:
        raise Fail("Plane 2 holds depths %d..%d, expected 9..9." % (two["min"], two["max"]))
    want = depth_stats(sweep_values(-2, 2, 3, three["frames"]))
    for key in ("min", "max", "changes", "frames"):
        if three.get(key) != want[key]:
            raise Fail("Plane 3 reports %s %r, expected %r."
                       % (key, three.get(key), want[key]))
    s.check_report_planes_empty(report, {2, 3}, "The created stream")
    return "Planes 2 and 3 created.", [job, out]


def sc_create_import_ofs(s):
    frames = s.frame_count
    if not frames:
        raise Skip("The frame count of the stream is unknown.")
    depths = [(1 + (i % 5)) * (-1 if (i // 5) % 2 else 1) for i in range(frames)]
    path = s.out(s.base("create_import_ofs", ".ofs"))
    write_ofs(path, 6, depths)
    out = s.out(s.base("create_import_ofs", ".mvc"))
    s.tool(["-quiet", "-from-ofs", path, "-base-view", s.base_view, s.input, out])
    report = s.report(out)
    got = plane_stat(report, 6, "The created stream")
    if got["min"] != min(depths) or got["max"] != max(depths):
        raise Fail("Plane 6 holds depths %d..%d, expected %d..%d."
                   % (got["min"], got["max"], min(depths), max(depths)))
    if got["frames"] != frames:
        raise Fail("Plane 6 holds %s, expected %s."
                   % (count(got["frames"], "frame"), count(frames, "frame")))
    s.check_report_planes_empty(report, {6}, "The created stream")
    return "%s imported into plane 6." % count(frames, "frame"), [path, out]


def sc_create_import_json(s):
    frames = s.frame_count
    if not frames:
        raise Skip("The frame count of the stream is unknown.")
    depths = [(1 + (i % 5)) * (-1 if (i // 5) % 2 else 1) for i in range(frames)]
    path = s.out(s.base("create_import_json", ".json"))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(plane_json_doc(7, depths), f, indent=2)
    out = s.out(s.base("create_import_json", ".mvc"))
    s.tool(["-quiet", "-from-json", path, "-base-view", s.base_view, s.input, out])
    report = s.report(out)
    got = plane_stat(report, 7, "The created stream")
    if got["min"] != min(depths) or got["max"] != max(depths):
        raise Fail("Plane 7 holds depths %d..%d, expected %d..%d."
                   % (got["min"], got["max"], min(depths), max(depths)))
    if got["frames"] != frames:
        raise Fail("Plane 7 holds %s, expected %s."
                   % (count(got["frames"], "frame"), count(frames, "frame")))
    s.check_report_planes_empty(report, {7}, "The created stream")
    return "%s imported into plane 7." % count(frames, "frame"), [path, out]


# Output name -> the scenario that writes it.
ARTEFACTS = {
    "analyze": "Analyze stream",
    "export_ofs": "Export to OFS",
    "export_ofs_no_force": "Export to OFS without -force",
    "export_json": "Export to JSON (single file)",
    "export_json_dir": "Export to JSON (file per plane)",
    "ofs_to_json": "Convert OFS to JSON",
    "json_to_ofs": "Convert JSON to OFS",
    "import_ofs": "Import OFS",
    "import_json": "Import JSON",
    "import_plane": "Import into a single plane",
    "import_list": "Import into multiple planes",
    "mismatch": "Frame-count mismatch",
    "depth_zero": "Depth 0 in plane file",
    "depth_option": "Depth 0 as write option",
    "export_fps4": "Export at frame rate 4",
    "export_single_plane": "Export a single plane",
    "create_depth": "Create plane with fixed depth",
    "create_sweep": "Create plane with sweeping depth",
    "create_batch": "Create planes from batch job",
    "create_import_ofs": "Create plane from OFS import",
    "create_import_json": "Create plane from JSON import",
}

# name -> (the scenario, what it checks, whether it writes a copy of the stream,
#          the scenarios that read its outputs)
SCENARIOS = {
    "Analyze stream": (sc_analyze,
                "Reads the stream's offset metadata and lists its planes.",
                False, ()),
    "Export to OFS": (sc_export_ofs,
                   "Exports each plane to an OFS 3D-Plane file.",
                   False, ("Convert OFS to JSON", "Convert JSON to OFS", "Import OFS",
                           "Import into a single plane", "Import into multiple planes")),
    "Export to OFS without -force": (sc_export_no_force,
                                  "Exports without -force and expects depth-0 frames to be rejected.",
                                  False, ()),
    "Export to JSON (single file)": (sc_export_json,
                                "Exports all planes to a single plane JSON file.",
                                False, ("Import JSON",)),
    "Export to JSON (file per plane)": (sc_export_json_dir,
                                          "Exports each plane to its own plane JSON file.",
                                          False, ()),
    "Convert OFS to JSON": (sc_ofs_to_json,
                    "Converts the exported OFS files to plane JSON and compares depths.",
                    False, ("Convert JSON to OFS",)),
    "Convert JSON to OFS": (sc_json_to_ofs,
                    "Converts plane JSON back to OFS and compares it with the original export.",
                    False, ()),
    "Import OFS": (sc_import_ofs,
                   "Imports the exported OFS planes back into the stream.",
                   True, ("Import JSON",)),
    "Import JSON": (sc_import_json,
                    "Imports plane JSON and compares the result with the OFS import.",
                    True, ()),
    "Import into a single plane": (sc_import_plane,
                              "Imports a single plane into its original plane ID.",
                              True, ()),
    "Import into multiple planes": (sc_import_list,
                                  "Imports two planes into their original plane IDs.",
                                  True, ()),
    "Frame-count mismatch": (sc_mismatch,
                             "Imports a plane with fewer frames; checks the mismatch is reported and can be resolved.",
                             True, ()),
    "Depth 0 in plane file": (sc_depth_zero,
                                "Imports a plane containing depth 0; expects rejection, and depth 1 with -force.",
                                True, ()),
    "Depth 0 as write option": (sc_depth_option,
                                    "Writes a plane at depth 0; expects rejection, and depth 1 with -force.",
                                    True, ()),
    "Export at frame rate 4": (sc_export_fps,
                                 "Exports planes at frame rate 4 with the drop-frame flag.",
                                 False, ()),
    "Export a single plane": (sc_export_single_plane,
                                 "Exports a single selected plane.",
                                 False, ()),
    "Create plane with fixed depth": (sc_create_depth,
                                   "Creates a plane with a fixed depth in a stream without metadata.",
                                   True, ()),
    "Create plane with sweeping depth": (sc_create_sweep,
                                "Creates a plane whose depth sweeps across frames.",
                                True, ()),
    "Create planes from batch job": (sc_create_batch,
                                       "Creates two planes from a batch job.",
                                       True, ()),
    "Create plane from OFS import": (sc_create_import_ofs,
                                       "Imports an OFS plane into a stream without metadata.",
                                       True, ()),
    "Create plane from JSON import": (sc_create_import_json,
                                       "Imports a plane JSON file into a stream without metadata.",
                                       True, ()),
}

# Scenarios that need a stream that already holds offset-metadata.
NEEDS_PLANES = (
    "Export to OFS", "Export to OFS without -force", "Export to JSON (single file)",
    "Export to JSON (file per plane)",
    "Convert OFS to JSON", "Convert JSON to OFS", "Import OFS", "Import JSON",
    "Import into a single plane", "Import into multiple planes",
    "Frame-count mismatch", "Depth 0 in plane file",
    "Export at frame rate 4", "Export a single plane",
)

# Scenarios that are only run on a stream that holds no metadata.
NEEDS_NO_PLANES = (
    "Create plane with fixed depth", "Create plane with sweeping depth",
    "Create planes from batch job",
    "Create plane from OFS import", "Create plane from JSON import",
)


# ── the run ──────────────────────────────────────────────────────────────
def detect(suite):
    """Find out which kind of input this is."""
    report = suite.analyze(suite.input)
    if report is not None:
        suite.kind = "planes"
        return "MVC stream containing 3D-planes."
    suite.kind = "no-planes"
    counts = parse_nal_histogram(suite.last_text)
    if counts.get(1, 0) + counts.get(5, 0) > 0:
        suite.base_view = suite.input
        return "MVC stream with a base view and no 3D-planes."
    found = find_base_view(suite.input)
    if found:
        suite.base_view = found
        return "MVC stream without 3D-planes; base view %s." % os.path.basename(found)
    return "MVC stream without 3D-planes or a base view."


def scenarios_for(suite, fast):
    """The scenarios that run with this input."""
    names = list(SCENARIOS)
    if suite.kind == "planes":
        names = [n for n in names if n not in NEEDS_NO_PLANES]
    else:
        names = [n for n in names if n not in NEEDS_PLANES]
    if fast:
        names = [n for n in names if not SCENARIOS[n][2]]
    return names


def estimate(suite, names, cleanup):
    """The space the run needs, the space it has and the number of copies."""
    size = os.path.getsize(suite.input)
    copies = sum(1 for n in names if SCENARIOS[n][2])
    peak = 2 if cleanup else max(copies, 1)
    needed = size * peak + 64 * 1024 * 1024
    drive = os.path.dirname(suite.results) or "."
    try:
        free = shutil.disk_usage(drive).free
    except OSError:
        free = 0
    return needed, free, copies


def confirm(needed, free, copies, cleanup, force):
    """Ask whether the run may start."""
    print("Space needed : %s" % human(needed))
    print("Space free   : %s" % human(free))
    if free and free < needed:
        print("Warning      : Not enough free space.")
    if force:
        print()
        return True
    if not sys.stdin.isatty():
        print("Not started: no terminal to ask; run with --force.")
        print()
        return False
    try:
        answer = input("Start the run? [Y/n] ")
    except (EOFError, KeyboardInterrupt):
        return 0
    if answer.strip().lower() in ("", "y", "yes"):
        print()
        return True
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Run the PlaneForge test suite against one input file.")
    ap.add_argument("input", help="The MVC stream to test")
    ap.add_argument("--exe", help="Path of the PlaneForge binary")
    ap.add_argument("--results", help="Folder for the test outputs "
                                      "(default: <input>_test-outputs next to the input)")
    ap.add_argument("--cleanup", action="store_true",
                    help="Delete test outputs as the run proceeds.")
    ap.add_argument("--fast", action="store_true",
                    help="Skip tests that write a full copy of the stream.")
    ap.add_argument("--json", dest="json_report", action="store_true",
                    help="Also write <input>_report.json.")
    ap.add_argument("--force", dest="force", action="store_true",
                    help="Start without asking for confirmation.")
    args = ap.parse_args()

    exe = args.exe
    if not exe:
        exe = find_tool()
    if not exe or not os.path.isfile(exe):
        print("PlaneForge binary not found; build it or pass --exe.", file=sys.stderr)
        return 2

    if not os.path.isfile(args.input):
        print("No such file: %s" % args.input, file=sys.stderr)
        return 2

    results = args.results or os.path.join(
        os.path.dirname(os.path.abspath(args.input)),
        os.path.splitext(os.path.basename(args.input))[0] + "_test-outputs")

    suite = Suite(exe, args.input, results, cleanup=args.cleanup)

    print("PlaneForge test suite")
    print("  Tool    : %s" % exe)
    print("  Input   : %s" % suite.input)
    print("  Results : %s" % suite.results)
    print()

    kind = detect(suite)
    print("Input: %s" % kind)
    print()

    names = scenarios_for(suite, args.fast)
    for i, name in enumerate(names):
        suite.index[name] = i + 1

    needed, free, copies = estimate(suite, names, args.cleanup)
    if not confirm(needed, free, copies, args.cleanup, args.force):
        return 2

    shutil.rmtree(suite.results, ignore_errors=True)
    os.makedirs(suite.results, exist_ok=True)

    width = max(len(n) for n in names)
    for i, name in enumerate(names):
        started = time.time()
        suite.scenario(name)
        row = suite.rows[-1]
        took = time.time() - started
        print("  %-*s  %-4s  %6.1fs  %s"
              % (width, name, row["status"], took, row["detail"]))
        if args.cleanup:
            suite.sweep(names, i)

    print()
    passed = sum(1 for r in suite.rows if r["status"] == "PASS")
    failed = sum(1 for r in suite.rows if r["status"] == "FAIL")
    skipped = sum(1 for r in suite.rows if r["status"] == "SKIP")
    for note in suite.notes:
        print("Note: %s" % note)

    if args.json_report:
        with open(os.path.join(os.path.dirname(suite.input), os.path.splitext(os.path.basename(suite.input))[0] + "_report.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)

    lines = ["PlaneForge test suite", "=" * 70,
             "Input    : %s" % suite.input,
             "Kind     : %s" % suite.kind,
             "Base view: %s" % (suite.base_view or "-"),
             "Results  : %s" % suite.results,
             "Tool     : %s" % exe, ""]
    for r in suite.rows:
        lines.append("%-5s %s" % (r["status"], r["name"]))
        lines.append("      Test   : %s" % r["description"])
        lines.append("      Result : %s" % r["detail"])
        if not r["artifacts"]:
            lines.append("      Output : None")
        for i, a in enumerate(r["artifacts"]):
            lines.append("      %s%s" % ("Output : " if i == 0 else " " * 9, a))
        lines.append("")
    if suite.notes:
        lines.append("Notes")
        for note in suite.notes:
            lines.append("  %s" % note)
        lines.append("")
    lines.append("-" * 70)
    lines.append("%d passed, %d failed, %d skipped." % (passed, failed, skipped))
    with open(os.path.join(os.path.dirname(suite.input), os.path.splitext(os.path.basename(suite.input))[0] + "_report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("%d passed, %d failed, %d skipped." % (passed, failed, skipped))
    print("Report: %s" % os.path.join(os.path.dirname(suite.input), os.path.splitext(os.path.basename(suite.input))[0] + "_report.txt"))

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
