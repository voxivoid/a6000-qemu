#!/usr/bin/env python3
"""avcam_gamma.py - evidence of Picture-Profile gamma support in an av-cam.bin.

The A6000's Picture Profiles (S-Log2, Cine, ITU709, knee, black level) are only
real if the body's image-processor firmware (av-cam.bin) actually carries the
gamma curves and the code that selects them.  This dumps the comparable
evidence from one av-cam so two bodies (A6000 vs a6300) can be diffed:

    tools/avcam_gamma.py <av-cam.bin>            # print a report
    tools/avcam_gamma.py <a.bin> <b.bin>         # side-by-side counts

Counts are what matter across bodies: more gamma-type / S-Log / knee markers in
the a6300 than the A6000 is the tell that the feature is firmware-gated there
and missing here.  Nothing here touches the camera or the NAND.
"""
import re
import sys

# markers that, if present, name a Picture-Profile gamma feature in the ISP fw
MARKERS = [
    rb"(?<![A-Za-z])[Ss]-?[Ll]og", rb"SLOG", rb"CINE", rb"[Cc]inema",
    rb"ITU[-_ ]?709", rb"ITU709",
    rb"STILL.?GAMMA", rb"MOVIE.?GAMMA", rb"GAMMA_?TYPE", rb"GammaType",
    rb"PROCOLOR", rb"[Pp]icture.?[Pp]rofile", rb"KNEE", rb"[Kk]nee",
    rb"BLACK.?GAMMA", rb"BlackGamma", rb"PEDESTAL", rb"COLOR_?MODE",
    rb"COLOR.?DEPTH", rb"DETAIL", rb"HLG", rb"HyperGamma", rb"[Cc]ine[0-9]",
]


def report(path):
    d = open(path, "rb").read()
    print("== %s (%d bytes) ==" % (path, len(d)))
    tot = {}
    for m in MARKERS:
        hits = re.findall(m, d)
        if hits:
            tot[m.decode()] = len(hits)
    for k in sorted(tot, key=lambda x: -tot[x]):
        print("  %-18s %d" % (k, tot[k]))
    if not tot:
        print("  (no gamma/PP markers found)")
    # gamma-related printable log lines, for eyeballing selection code
    lines = re.findall(rb"[ -~]{6,}", d)
    gl = sorted(set(l.decode() for l in lines
                    if re.search(rb"[Gg]amma|GAMMA|[Ss].?[Ll]og|CINE|KNEE|709", l)))
    print("  -- %d distinct gamma/log strings; sample:" % len(gl))
    for s in gl[:25]:
        print("    " + s[:100])
    return tot


def main():
    if len(sys.argv) == 2:
        report(sys.argv[1])
    elif len(sys.argv) == 3:
        a = report(sys.argv[1])
        print()
        b = report(sys.argv[2])
        print("\n== diff (marker: A -> B) ==")
        for k in sorted(set(a) | set(b)):
            av, bv = a.get(k, 0), b.get(k, 0)
            flag = "  <-- differs" if av != bv else ""
            print("  %-18s %d -> %d%s" % (k, av, bv, flag))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
