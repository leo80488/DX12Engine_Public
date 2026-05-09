#pragma once

// Font Awesome 4/5/6 icon codepoints (Private Use Area) and UTF-8 strings for ImGui.
// Use with fa-solid-900.ttf (place in external/fontawesome/).
// Play = U+F04B, Pause = U+F04C, Stop = U+F04D, StepForward = U+F051

#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace FontAwesome
{
    // Codepoint ranges for glyph atlas.  Pairs: {min, max, 0-terminator}.
    // Keep these in ascending order — ImGui requires sorted, non-overlapping pairs.
    inline const unsigned int kIconRangeMin = 0xf003;  // fa-envelope (earliest needed)
    inline const unsigned int kIconRangeMax = 0xf1fc;  // fa-paint-brush (latest needed)

    // --- Asset-browser icons ------------------------------------------------
    inline const char* kUser         = "\xef\x80\x87";  // U+F007  fa-user        (skeleton/character)
    inline const char* kFilm         = "\xef\x80\x88";  // U+F008  fa-film        (animation clip)
    inline const char* kFolder       = "\xef\x81\xbb";  // U+F07B  fa-folder
    inline const char* kFolderOpen   = "\xef\x81\xbc";  // U+F07C  fa-folder-open
    inline const char* kImage        = "\xef\x80\xbe";  // U+F03E  fa-image
    inline const char* kVolumeUp     = "\xef\x80\xa8";  // U+F028  fa-volume-up   (audio)
    // --- Viewport toolbar icons ---------------------------------------------
    inline const char* kPlay         = "\xef\x81\x8b";  // U+F04B  fa-play
    inline const char* kPause        = "\xef\x81\x8c";  // U+F04C  fa-pause
    inline const char* kStop         = "\xef\x81\x8d";  // U+F04D  fa-stop
    inline const char* kStepForward  = "\xef\x81\x91";  // U+F051  fa-step-forward
    // --- More asset-browser icons -------------------------------------------
    inline const char* kFileText     = "\xef\x83\xb6";  // U+F0F6  fa-file-text-o (script)
    inline const char* kCode         = "\xef\x84\xa1";  // U+F121  fa-code       (shader)
    inline const char* kCube         = "\xef\x86\xb2";  // U+F1B2  fa-cube       (mesh)
    inline const char* kCubes        = "\xef\x86\xb3";  // U+F1B3  fa-cubes      (scene)
    inline const char* kPaintBrush   = "\xef\x87\xbc";  // U+F1FC  fa-paint-brush (material)
}
