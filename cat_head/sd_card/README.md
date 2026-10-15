# SD card contents

Copy these folders to the root of a FAT32 microSD card and put `.wav` files in them.
One random file from the folder is played each time (so add several for variety).

| Folder      | Played when                                 |
|-------------|---------------------------------------------|
| `/meow`     | tapped, waking up, random idle meow         |
| `/purr`     | being petted (held) – looped                |
| `/hiss`     | tapped too many times (annoyed)             |
| `/yawn`     | getting sleepy, waking up on its own        |
| `/surprise` | bottom layer hits an obstacle               |

Format: **PCM 16-bit WAV**, mono or stereo, 16–44.1 kHz (22050 Hz mono is plenty).
Convert anything with ffmpeg:

    ffmpeg -i input.mp3 -ac 1 -ar 22050 -sample_fmt s16 meow1.wav

Avoid long file names with spaces; short names like `meow1.wav` are safest.
