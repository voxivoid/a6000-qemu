# ISP look presets (JSON) for `mods/look/ispctl.py`
Every key is optional; only present groups are frozen and written.

```json
{ "name": "...",
  "gamma":      {"look": "slog2|slog3|logc3|arri709|rec709|identity|gamma", "grey_x": 76, "range": "legal|full", "g": 2.2}   // or {"file": "path/to/96xu32.bin"}
  "matrix":     "stock|identity|awg3|rec2020|p3|slg3"  |  {"mode": "sat", "value": 0.8} | {"mode":"custom","matrix":[[1024,0,0],[0,1024,0],[0,0,1024]]},
  "saturation": 0.0..2.0 (scale on the camera's stock chroma gains; 0 = mono)  |  [g0, g1, g2, g3]  (absolute, stock ~93,99,79,90),
  "sharpness":  0..15 (stock Standard ~12),
  "wb":         {"r_scale": 1.0, "b_scale": 1.0}   // tint relative to stock R/B gains (stock = values at first apply)
}
```
Notes: stock values are captured the first time you apply (all setters original) into `.state_stock.json` (git-ignored). WB values are frozen at that moment (no auto-WB
tracking while the preset is active). Matrix targets are applied on top of the camera's own sensor->Rec.709 matrix. Evidence for every register: docs/A6000_ISP_REGISTER_MAP.md.
