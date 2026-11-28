# Shield DJ Standard by Shield-77

Current source: v1.0.38 (CIA title version 1062). The notes below also cover earlier versions.

Version 1.0.36 adds an optional long reverb decay alongside the existing
custom/preset switch in Options. It is off by default. Playback time now
follows the DSP rate actually applied during pitch changes, including the
short rate ramp and fractional milliseconds between UI frames.

Version 1.0.35 adds an Options screen with seven audio filters (None, Rock,
Pop, Jazz, Clear, Outdoor, Classical) above a touch reverb bar. The bar shows
four presets or a free 0-100% control according to its mode. Changes apply
when the finger is released. SELECT opens or closes Options; A and touch select
a setting, B closes the screen. Audio playback continues while navigating.
The reverb tails and gain changes are smoothed, and selecting Bass OFF now
disables the bass biquad rather than applying an undocumented boost.

Version 1.0.29 embeds a higher-resolution semibold UI font for the top screen.
It preserves the existing layout while drawing text from a 40 px glyph atlas at
the same displayed size, reducing coarse edges on the 400x240 panel.

Version 1.0.25 adds a +16 dB bass profile between Boost (+9 dB) and Extreme (+22 dB), selectable with Y during playback. The DSP tone filter and existing soft clipping remain in use.

Version 1.0.24 raises only the Extreme bass profile to +22 dB. The other three profiles remain at +3, +6 and +9 dB. DSP soft clipping stays enabled and the full-band bass preamp stays removed. Loud tracks may still distort at this extreme setting, particularly with audio boost.

Version 1.0.23 removes the full-band bass preamp that muffled loud tracks, uses short-window audio boost, smooths DSP rate changes for pitch, and preserves reverb tails when changing active profiles. DSP soft clipping remains enabled to handle bass peaks.

Version 1.0.22 estimates bass headroom at each instant and smooths gain
recovery within PCM blocks for bass protection and audio boost. Bass analysis
state resets between tracks. Short recoverable OGG read errors are retried,
as in Lite, instead of immediately stopping playback.

Version 1.0.21 retunes bass around 180 Hz and bases gain protection on actual
bass-band energy. This gives kick drums more presence while avoiding needless
attenuation of vocals and treble in all four profiles. The displayed profile
gains remain +3, +6, +9 and +12 dB.

Standard edition of Shield DJ for the 3DS platform. Its title ID is
`000400000A210300`, so it installs alongside Light (`000400000A210100`).

Both screens use Citro2D/Citro3D with navy/cyan panels, small effect icons
and physical button badges. The top screen uses an embedded semibold font
rendered from its higher-resolution 40 px atlas; the lower screen keeps the
filtered system font and its existing layout.
The top screen
shows cover art, title/artist/album when metadata is available, progress, and
playback settings. The lower screen shows the SD library with music/folder
icons, spacing, a selected row, known track durations, and physical controls.
MP3 ID3 APIC JPEG artwork is center-cropped to a 128x128 RGB565 cache and rotates at a
rate linked to pitch; it stops when paused and resumes with playback. The cover
fills the disc while leaving its outer rim visible. If a track has no supported
cover, the UI shows a vinyl record with the transparent SHIELD emblem in its
center. The record spins at 10 RPM at normal playback speed and remains linked
to pitch. Its rings are fixed UI circles; the center label rotates without
decorative markers on the grooves. The animation is paced by VBlank
at about 20 frames per second, and the lower screen is not redrawn for record
frames. L+R
toggles the previous/next-track lock.

Playback, pitch linked to speed, bass profiles, reverb, shuffle, brightness,
battery/temperature reporting, and SD reporting are carried over from Light.
The battery temperature is shown with a degree symbol (for example, 26°C).
On the top screen, a flashing red alert overlays all other UI when the battery
display reaches 20% without a detected charger. A hotter battery takes priority:
at 40°C on Old 2DS or 42°C on Old 3DS/XL, it shows an urgent shutdown warning.
New-model and unknown-model thresholds are 45°C and 42°C respectively.
The first crossing plays a two-second "beep beep" pattern on a separate NDSP
channel that follows the console's normal headphone/speaker output. At the
thermal threshold, playback stops and cannot restart until a fresh cached
temperature reading falls below the threshold. Playback then requires a manual
restart. Alerts use the existing cached sensor readings; battery,
charger and temperature polling rates are unchanged.
The D-pad changes pitch in 0.02x steps; holding left or right repeats the
change until release, within the 0.25x to 1.75x range. Circle Pad input is
ignored; navigation, brightness and pitch use the physical D-pad only.
HOME cycles the audio boost through off, 200%, 300% and 500%, with a peak cap
to avoid digital clipping. X+SELECT toggles repeat for the current track;
the original X shuffle and SELECT reverb actions remain available.
The five bass profiles are +3, +6, +9, +16 and +22 dB. DSP soft clipping handles
peaks; loud tracks can still sound compressed with Extreme and audio boost.
MP3 playback starts after its ID3 metadata, so large embedded covers cannot
be mistaken for damaged audio. Switching shuffle off keeps the current track
position, and a reshuffle avoids immediately repeating the last track.
Selecting a file uses the displayed SD listing directly, preventing a fresh
directory scan from shifting the selection to another song.
While the lid is closed, cover decoding and animation are suspended; audio
buffering and next-track handling continue.

The CIA banner uses the supplied SHIELD DJ STANDARD artwork adapted to the
3DS 256x128 banner canvas. The Old 3DS/2DS CPU and memory limits remain the
target. The cover cache uses 32 KiB plus a 64 KiB upload buffer and a 64 KiB
GPU-readable linear texture. The header logo texture uses 16 KiB. The startup
texture uses 256 KiB of linear memory, released before the library is shown
(the packaged asset remains in the executable). Embedded images larger than 256 KiB or
non-JPEG APIC images fall back to the record graphic.

Startup shows the supplied SHIELD artwork for at least 2.5 seconds, then a
three-page first-use guide
on the top screen. A advances pages; A on page 3 acknowledges the guide and
writes `sdmc:/3ds/ShieldDJStandard/guide-v1.dat`. Subsequent launches skip it.
START can exit the guide without marking it read. The SD must be writable to
remember acknowledgement. No playback starts while acknowledging a page.

The render regression (visible panels but hidden text/disc) was caused by
inconsistent depths. All UI draws now share one painter-ordered plane.
PICA texture byte order and artwork UV coordinates are corrected. Cover
uploads occur only after the previous GPU frame has finished. Drawing budget
is 1536 objects for both screens, including glyphs. Audio/DSP and playback
controls retain their implementation.

Original artwork is in `assets/`; `tools/pack_ui_assets.py` builds the bundled
texture header with Python + Pillow. The normal build consumes that header
and does not require Python. `tools/preview_ui.py` produces a desktop layout
preview with example metadata and a substitute font, not a GPU screenshot.
Differences from the concept: the physical command help uses two rows plus
a small reminder. Battery and temperature have their own line below the
button guide. The SD and brightness indicators are retained; artwork depends
on the track's supported embedded cover.

The top font is the bundled `ShieldUISans40-Semibold.bcfnt`, rendered with
linear filtering and scaled to preserve the established layout. It occupies
about 1 MiB in the executable and does not require a TTF on the SD card. Its
source and license remain in `assets/fonts/`.

Compile with `make cia` from a devkitPro 3DS shell. Output:
`build/ShieldDJStandard.cia`.
