# Vendored single-header libraries

`dr_wav.h` and `dr_mp3.h` are David Reid's public domain / MIT-0 decoders
(https://github.com/mackron/dr_libs), used by the Soundboard
(`engine/soundboardengine.cpp`) to decode `.wav` and `.mp3` files without
requiring a system audio-decoding library. See
`../../../LICENSES/dr_libs-Unlicense-MIT0.txt`.

Only the implementation is compiled where it is needed
(`DR_WAV_IMPLEMENTATION` / `DR_MP3_IMPLEMENTATION`, defined once in
`soundboardengine.cpp`); every other includer sees declarations only.
