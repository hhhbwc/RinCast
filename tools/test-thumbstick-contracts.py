#!/usr/bin/env python3
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
cpp = repo / "headset/app/src/main/cpp"
app = (cpp / "app/app.cpp").read_text(encoding="utf-8")
xr = (cpp / "xr/xr_input.cpp").read_text(encoding="utf-8")
screens = (cpp / "app/screens.cpp").read_text(encoding="utf-8")
settings = (cpp / "app/settings.cpp").read_text(encoding="utf-8")

checks = {
    "OpenXR keeps raw thumbstick values": "h.thumbstick = h.thumbstickActive ? Vec2{v.currentState.x, v.currentState.y} : Vec2{};" in xr,
    "thumbstick capability comes from vector action state": "h.thumbstickActive = XR_SUCCEEDED(stickResult) && v.isActive;" in xr,
    "raw input is not filtered at acquisition": "ApplyThumbstickDeadzone" not in xr,
    "all three consumers use the shared filter": app.count("ApplyThumbstickDeadzone(") == 3,
    "UI uses a radial smooth gate": "uiStick.y * SmoothStep(0.12f, 0.20f, uiMag)" in app,
    "desktop uses the active mouse hand": "m_settings.StickDeadzone(mouseHand)" in app,
    "SteamVR uses the physical hand": "m_settings.StickDeadzone(h)" in app,
    "visualizer requires actual thumbstick capability": "hand.thumbstickActive && !hand.isHand" in screens,
    "raw point flips OpenXR Y for UI": "rawY = cy - raw.y * rawScale * radius" in screens,
    "output point flips OpenXR Y for UI": "outY = cy - out.y * radius" in screens,
    "controllers page is registered": "case kControllers: PageControllers(ui, body); break;" in screens,
    "calibration conflicts are disabled": "!calibrating && canCalibrate" in screens,
    "calibration checks termination after every sample": "calibration.Observe(m_input.Hand(hand).thumbstick);\n        if (calibration.active) continue;\n        if (!calibration.CanApply())" in app,
    "independent calibration failures stay per-hand": app.count("if (linked) break;") == 2,
    "cards show per-hand calibration progress": all(token in screens for token in ("StickCalibrationActive(0)", "StickCalibrationActive(1)", "StickCalibrationProgress(0)", "StickCalibrationProgress(1)")),
    "new settings are persisted": all(f'"{key}"' in settings for key in ("leftStickDeadzone", "rightStickDeadzone", "linkStickDeadzone")),
}
stream_body = settings.split("json HeadsetSettings::StreamSettingsJson() const {", 1)[1].split("\n}", 1)[0]
checks["stream handshake excludes local deadzone settings"] = all(key not in stream_body for key in ("leftStickDeadzone", "rightStickDeadzone", "linkStickDeadzone"))

for name, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'} {name}")
if not all(checks.values()):
    sys.exit(1)
print(f"ALL {len(checks)} THUMBSTICK CONTRACT CHECKS PASSED")
