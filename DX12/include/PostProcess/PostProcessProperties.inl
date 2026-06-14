// PostProcessProperties.inl — SINGLE SOURCE OF TRUTH for every post-process
// property exposed by the volume/profile system. Adding a property here makes
// it appear automatically in:
//   - PostProcessProfile         (as Overridable<T>, member <group>.<member>)
//   - ResolvedPostProcessSettings(as bare T,          member <group>.<member>)
//   - MakeEngineDefaultProfile() / Resolved default ctor   (default value)
//   - BlendProfileInto() / Flatten()                       (resolve loop)
//   - PostProcessProfileSerializer .ppprofile read/write   (key "<group>_<member>")
//   - the editor profile inspector                         (override checkbox + widget)
//
// This file is RE-INCLUDABLE. Each consumer #defines whichever of the PP_*
// macros it cares about *before* #include-ing this file; the rest default to
// no-ops, and ALL of them are #undef-ed again at the bottom so the next
// include starts clean.
//
// Macro forms:
//   PP_GROUP(group, label)                       — UI section header
//   PP_BOOL  (group, member, label, def)
//   PP_FLOAT (group, member, label, def, uiMin, uiMax)
//   PP_FLOAT3(group, member, label, defX, defY, defZ)   — 3-float drag
//   PP_COLOR (group, member, label, defR, defG, defB)   — color3 picker
//   PP_UINT  (group, member, label, def, uiMin, uiMax)

#ifndef PP_GROUP
#define PP_GROUP(group, label)
#endif
#ifndef PP_BOOL
#define PP_BOOL(group, member, label, def)
#endif
#ifndef PP_FLOAT
#define PP_FLOAT(group, member, label, def, uiMin, uiMax)
#endif
#ifndef PP_FLOAT3
#define PP_FLOAT3(group, member, label, defX, defY, defZ)
#endif
#ifndef PP_COLOR
#define PP_COLOR(group, member, label, defR, defG, defB)
#endif
#ifndef PP_UINT
#define PP_UINT(group, member, label, def, uiMin, uiMax)
#endif

// ---- Contrast Adaptive Sharpening (HDR, pre-exposure) ----------------------
PP_GROUP(cas, "Contrast Adaptive Sharpening")
PP_BOOL (cas, enabled,   "Enabled",   true)
PP_FLOAT(cas, sharpness, "Sharpness", 0.6f, 0.0f, 1.0f)

// ---- Auto Exposure (histogram EV) -----------------------------------------
PP_GROUP(autoExposure, "Auto Exposure")
PP_BOOL (autoExposure, enabled,        "Auto Exposure",   true)
PP_FLOAT(autoExposure, manualExposure, "Manual Exposure", 1.0f, 0.0f, 16.0f)
PP_FLOAT(autoExposure, adaptationTau,  "Adaptation Tau",  1.5f, 0.0f, 10.0f)
PP_FLOAT(autoExposure, minLogLuma,     "Min Log Luma",   -5.0f, -10.0f, 5.0f)
PP_FLOAT(autoExposure, maxLogLuma,     "Max Log Luma",    3.5f, -5.0f, 10.0f)
PP_FLOAT(autoExposure, lowPercent,     "Low Percentile",  0.50f, 0.0f, 1.0f)
PP_FLOAT(autoExposure, highPercent,    "High Percentile", 0.85f, 0.0f, 1.0f)
PP_FLOAT(autoExposure, minExposure,    "Min Exposure",    0.10f, 0.0f, 8.0f)
PP_FLOAT(autoExposure, maxExposure,    "Max Exposure",    8.0f, 0.0f, 32.0f)
PP_FLOAT(autoExposure, evBias,         "EV Bias",         0.0f, -8.0f, 8.0f)
PP_FLOAT(autoExposure, keyValue,       "Key Value",       0.18f, 0.0f, 1.0f)

// ---- Bloom ----------------------------------------------------------------
PP_GROUP(bloom, "Bloom")
PP_FLOAT(bloom, strength, "Bloom Strength", 0.04f, 0.0f, 1.0f)

// ---- Color Grading (baked into the tonemap 3D LUT) ------------------------
PP_GROUP(colorGrading, "Color Grading")
PP_BOOL (colorGrading, enabled,          "Color Grading", true)
PP_FLOAT(colorGrading, exposure,         "Exposure (EV)", 0.0f, -8.0f, 8.0f)
PP_FLOAT(colorGrading, contrast,         "Contrast",      1.0f, 0.0f, 2.0f)
PP_FLOAT(colorGrading, brightness,       "Brightness",    0.0f, -1.0f, 1.0f)
PP_COLOR(colorGrading, lift,             "Lift",          0.0f, 0.0f, 0.0f)
PP_COLOR(colorGrading, gamma,            "Gamma",         1.0f, 1.0f, 1.0f)
PP_COLOR(colorGrading, gain,             "Gain",          1.0f, 1.0f, 1.0f)
PP_FLOAT(colorGrading, hueShift,         "Hue Shift",     0.0f, -180.0f, 180.0f)
PP_FLOAT(colorGrading, saturation,       "Saturation",    1.0f, 0.0f, 2.0f)
PP_FLOAT(colorGrading, vibrance,         "Vibrance",      0.0f, -1.0f, 1.0f)
PP_FLOAT(colorGrading, temperature,      "Temperature",   0.0f, -1.0f, 1.0f)
PP_FLOAT(colorGrading, tint,             "Tint",          0.0f, -1.0f, 1.0f)
PP_FLOAT(colorGrading, vignetteStrength, "Vignette",      0.0f, 0.0f, 1.0f)
PP_FLOAT(colorGrading, filmGrain,        "Film Grain",    0.0f, 0.0f, 1.0f)

// ---- Lens Flare (procedural directional-light flare) ----------------------
PP_GROUP(lensFlare, "Lens Flare")
PP_BOOL (lensFlare, enabled,         "Lens Flare",       true)
PP_FLOAT(lensFlare, intensity,       "Intensity",        0.2f, 0.0f, 4.0f)
PP_UINT (lensFlare, ghostCount,      "Ghost Count",      6, 0, 16)
PP_FLOAT(lensFlare, ghostDispersal,  "Ghost Dispersal",  0.18f, 0.0f, 1.0f)
PP_FLOAT(lensFlare, haloWidth,       "Halo Width",       0.32f, 0.0f, 1.0f)
PP_FLOAT(lensFlare, streakLength,    "Streak Length",    0.55f, 0.0f, 2.0f)
PP_FLOAT(lensFlare, chromaticOffset, "Chromatic Offset", 0.012f, 0.0f, 0.1f)

// ---- Underwater (screen distortion + water tint, HDR pre-tonemap) ----------
PP_GROUP(underwater, "Underwater")
PP_BOOL (underwater, enabled,    "Underwater",  false)
PP_FLOAT(underwater, strength,   "Distortion",  0.012f, 0.0f, 0.1f)
PP_FLOAT(underwater, scale,      "Wave Scale",  28.0f, 1.0f, 120.0f)
PP_FLOAT(underwater, speed,      "Wave Speed",  1.5f, 0.0f, 8.0f)
PP_COLOR(underwater, tint,       "Water Tint",  0.45f, 0.75f, 0.85f)
PP_FLOAT(underwater, tintAmount, "Tint Amount", 0.6f, 0.0f, 1.0f)

// ---- Depth of Field (focus-distance CoC blur, HDR pre-tonemap) -------------
PP_GROUP(dof, "Depth of Field")
PP_BOOL (dof, enabled,         "Depth of Field",   false)
PP_FLOAT(dof, focusDistance,   "Focus Distance",   8.0f, 0.1f, 500.0f)
PP_FLOAT(dof, focusRange,      "Focus Range",      2.0f, 0.0f, 100.0f)
PP_FLOAT(dof, transitionRange, "Transition Range", 10.0f, 0.1f, 200.0f)
PP_FLOAT(dof, maxRadius,       "Max Blur (px)",    8.0f, 0.0f, 32.0f)

// ---- Stylize: Pixelation (snap to a coarse block grid) --------------------
PP_GROUP(pixelate, "Stylize: Pixelate")
PP_BOOL(pixelate, enabled, "Pixelate",       false)
PP_UINT(pixelate, size,    "Block Size (px)", 8, 1, 64)

// ---- Stylize: Kuwahara (painterly edge-preserving smoothing) --------------
PP_GROUP(kuwahara, "Stylize: Kuwahara")
PP_BOOL(kuwahara, enabled, "Kuwahara", false)
PP_UINT(kuwahara, radius,  "Radius",   3, 1, 7)

// ---- Stylize: Posterization (level quantization) --------------------------
PP_GROUP(posterize, "Stylize: Posterize")
PP_BOOL(posterize, enabled, "Posterize", false)
PP_UINT(posterize, levels,  "Levels",    6, 2, 32)

// ---- Stylize: Halftone (screen-tone dots) ---------------------------------
PP_GROUP(halftone, "Stylize: Halftone")
PP_BOOL (halftone, enabled,  "Halftone",       false)
PP_FLOAT(halftone, cellSize, "Cell Size (px)", 6.0f, 2.0f, 32.0f)
PP_FLOAT(halftone, angle,    "Screen Angle",   45.0f, 0.0f, 90.0f)

// ---- Stylize: Dithering (Bayer ordered dither) ----------------------------
PP_GROUP(dither, "Stylize: Dither")
PP_BOOL(dither, enabled, "Dither", false)
PP_UINT(dither, levels,  "Levels", 4, 2, 16)

// ---- Stylize: Crosshatching (ink hatch by luminance) ----------------------
PP_GROUP(crosshatch, "Stylize: Crosshatch")
PP_BOOL (crosshatch, enabled,   "Crosshatch", false)
PP_FLOAT(crosshatch, density,   "Density",    0.12f, 0.02f, 0.5f)
PP_FLOAT(crosshatch, thickness, "Thickness",  0.4f, 0.05f, 0.9f)

#undef PP_GROUP
#undef PP_BOOL
#undef PP_FLOAT
#undef PP_FLOAT3
#undef PP_COLOR
#undef PP_UINT
