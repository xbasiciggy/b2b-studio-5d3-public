"""ml-dev/cameras.txt -> the camera tables in README.md, README.th.md and TESTING.md, and the
cameras CI builds (.github/workflows/camera.yml). Run after changing cameras.txt:

    python3 ml-dev/tools/cameras.py
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATUS = {
    "working": "✅ **Works**",
    "testing": "🧪 Builds, needs a tester",
    "planned": "⏳ Planned",
    "no": "— Not yet",
}
WHY_NO = "Magic Lantern itself is still experimental on these"
# README.th.md
STATUS_TH = {
    "working": "✅ **ใช้ได้**",
    "testing": "🧪 build ได้แล้ว รอคนทดสอบ",
    "planned": "⏳ วางแผนไว้",
}
WHY_TH = {
    WHY_NO: "Magic Lantern เองยังเป็นรุ่นทดลองบนกล้องเหล่านี้",
    "Magic Lantern's current source doesn't build it": "source ปัจจุบันของ Magic Lantern build รุ่นนี้ไม่ได้",
}


def cameras():
    out = []
    for line in (ROOT / "ml-dev" / "cameras.txt").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        plat, status, limit, name = line.split(None, 3)
        fw = plat.split(".")[1]
        out.append({"plat": plat, "status": status, "limit": limit, "name": name,
                    "fw": ".".join(fw) if fw.isdigit() else fw})
    return out


def short(name):
    """'Canon EOS 200D / Rebel SL2' -> '200D', 'Canon PowerShot SX70 HS' -> 'SX70 HS'"""
    name = name.split(" / ", 1)[0]
    return re.sub(r"^Canon (EOS |PowerShot )?", "", name)


def replace(path, start, end, body):
    text = path.read_text(encoding="utf-8")
    a, b = text.index(start) + len(start), text.index(end)
    path.write_text(text[:a] + "\n" + body.rstrip("\n") + "\n" + text[b:], encoding="utf-8")


def main():
    cams = cameras()
    def table(head, status, not_yet, why_text, end="."):
        rows = [head, "|---|---|---|---|"]
        for c in cams:
            if c["status"] in ("working", "testing", "planned"):
                dl = f"`M-Film-camera-{c['plat']}.zip`" if c["status"] != "planned" else ""
                rows.append(f"| {c['name']} | {c['fw']} | {status[c['status']]} | {dl} |")
        # the ones Magic Lantern isn't ready on: one line, grouped by why
        why = {}
        for c in cams:
            if c["status"] == "no":
                name, _, reason = c["name"].partition(" (")
                why.setdefault(reason.rstrip(")") or WHY_NO, []).append(short(name))
        if why:
            rows += ["", f"**{not_yet}:** " + " · ".join(f"{', '.join(n)} ({why_text(r)})" for r, n in why.items()) + end]
        return "\n".join(rows)

    rows = table("| Camera | Canon firmware | Status | Card files |", STATUS, "Not yet", lambda r: r)
    th = table("| กล้อง | เฟิร์มแวร์ Canon | สถานะ | ไฟล์ลงการ์ด |", STATUS_TH, "ยังไม่รองรับ", lambda r: WHY_TH.get(r, r), end="")
    replace(ROOT / "README.th.md", "<!-- cameras:start -->", "<!-- cameras:end -->", th)
    replace(ROOT / "README.md", "<!-- cameras:start -->", "<!-- cameras:end -->", rows)

    built = [c for c in cams if c["status"] in ("working", "testing")]
    rows = ["| Camera | Canon firmware it needs | File |", "|---|---|---|"]
    rows += [f"| {c['name']} | {c['fw']} | `M-Film-camera-{c['plat']}.zip` |" for c in built]
    replace(ROOT / "TESTING.md", "<!-- cameras:start -->", "<!-- cameras:end -->", "\n".join(rows))

    wf = ROOT / ".github" / "workflows" / "camera.yml"
    text = wf.read_text(encoding="utf-8")
    text = re.sub(r"(        platform: )\[[^\]]*\]( *# from ml-dev/cameras.txt)?",
                  lambda m: m.group(1) + "[" + ", ".join(c["plat"] for c in built) + "]  # from ml-dev/cameras.txt", text, count=1)
    wf.write_text(text, encoding="utf-8")
    print(f"{len(cams)} cameras: {sum(c['status'] == 'working' for c in cams)} working, "
          f"{sum(c['status'] == 'testing' for c in cams)} testing")


if __name__ == "__main__":
    main()
