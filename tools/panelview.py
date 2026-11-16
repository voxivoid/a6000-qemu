#!/usr/bin/env python3
# panelview.py -- render a 640x480 a5000 OSD framebuffer as the PHYSICAL PANEL shows it.
#
# WHY: the a5000 panel driver horizontally STRETCHES the 640x480 OSD framebuffer to a
# 853x480 (16:9) display (the inverse of the "X0.75 logical squeeze" that uicore / Track A
# apply when authoring). The QEMU HMP `screendump` and the host-sim PPM both dump the RAW
# 640x480 framebuffer -- i.e. the PRE-stretch image -- so a geometry error that only shows
# up AFTER the panel stretch is invisible in a plain screendump. This tool applies the same
# 640 -> 853 horizontal linear stretch so what you view matches the real panel.
#
# Usage:
#   panelview.py IN [OUT]          # IN = .ppm (P6) or any PIL-readable image; OUT defaults to IN + .panel.png
#   panelview.py IN OUT --width N  # override target width (default 853)
#
# The inverse (authoring) factor is 640/853 = 0.7503 ("X0.75"); this tool does the forward
# 853/640 = 1.3328 stretch. Height is unchanged (480). Nearest/linear selectable.
import sys, os, struct

def read_ppm_p6(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None
    # parse header: P6 <w> <h> <maxval> then binary
    idx = 2
    vals = []
    while len(vals) < 3:
        # skip whitespace / comments
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b"#":
            while idx < len(data) and data[idx:idx+1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, mx = vals
    idx += 1  # single whitespace after maxval
    px = data[idx:idx + w*h*3]
    return w, h, px

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        print(__doc__); sys.exit(2)
    inp = args[0]
    out = args[1] if len(args) > 1 else inp + ".panel.png"
    width = 853
    interp = "linear"
    i = 0
    while i < len(opts):
        if opts[i] == "--width" and i+1 < len(sys.argv):
            # width is an opt value passed as separate arg; re-scan raw argv
            pass
        i += 1
    # simple flag parse over raw argv
    rv = sys.argv[1:]
    if "--width" in rv:
        width = int(rv[rv.index("--width")+1])
    if "--nearest" in rv:
        interp = "nearest"

    from PIL import Image
    p6 = read_ppm_p6(inp)
    if p6:
        w, h, px = p6
        img = Image.frombytes("RGB", (w, h), px)
    else:
        img = Image.open(inp).convert("RGB")
        w, h = img.size

    resample = Image.NEAREST if interp == "nearest" else Image.BILINEAR
    stretched = img.resize((width, h), resample)
    stretched.save(out)
    print("panelview: %s (%dx%d) -> %s (%dx%d, %s stretch x%.4f)" %
          (inp, w, h, out, width, h, interp, width / float(w)))

if __name__ == "__main__":
    main()
