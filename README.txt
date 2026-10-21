FFTool - CoD World at War (PS3) fastfile unpacker / repacker, byte-exact

DRAG AND DROP onto FFTool.exe:
  nazi_zombie_prototype.ff         -> nazi_zombie_prototype_unpacked\
                                        nazi_zombie_prototype.zone  (fully decompressed zone)
                                        ffinfo.bin                  (header/trailer, needed to rebuild)
                                        rawfiles\maps\*.gsc ...     (extracted scripts, editable)
                                        models\*.obj               (95 models, editable in Blender)
                                        models\_index.txt          (model list + which ones are skin variants)
                                        textures\*.dds             (680 textures, all formats + mipmaps)
                                        sounds\sfx\...\*.mp3       (1311 sounds: weapons, zombie voices, ...)
  nazi_zombie_prototype_unpacked   -> nazi_zombie_prototype_rebuilt.ff
  (or the .zone inside it)            If the original .ff sits next to it, the tool compares them
                                      and says IDENTICAL when nothing was changed.

  sound0.pak                       -> sound0_pak_unpacked\
                                        files\XXXXXXXX.mp3         (19,035 sounds, named by hash)
                                        files\_index.txt           (list of entries)
                                        pakinfo.bin                 (header/table/padding, needed to rebuild)
  sound0_pak_unpacked              -> sound0_rebuilt.pak  (IDENTICAL if nothing replaced)

REPLACING SOUNDS (.pak)
  Overwrite any file in files\ with your own sound (keep the same file name and
  the same format, e.g. MP3 for .mp3). Any size is fine - the table is rebuilt.
  Do not rename, add or delete files.

EDITING SOUNDS (.ff)
  sounds\sfx\weapon\...  = gun sounds,  sounds\sfx\levels\zombie\new_zombie_vox\... = zombie voices.
  Replace any .mp3 with your own MP3 (same file name) and rebuild. It must not be
  bigger than the original (sounds\_index.txt lists the max bytes) - export at a
  lower bitrate (64-96 kbps) and/or mono to fit. Sample rate 48000 Hz recommended.
  Music and battle chatter are streamed from sound0.pak, not stored in the .ff.

EDITING TEXTURES (.ff)
  Open a .dds in paint.net (free), GIMP or Photoshop (with a DDS plugin), edit it and
  save it over the same file in the SAME format, with mipmaps. You can also change the
  resolution (e.g. 256x256 -> 512x512) - any power-of-two size up to 4096 works:
    DXT1 -> "BC1 / DXT1",  DXT5 -> "BC3 / DXT5",  DXT3 -> "BC2 / DXT3",
    32-bit ones -> "A8R8G8B8 / uncompressed".   textures\_index.txt lists each format.
  Drop the _unpacked folder on FFTool.exe. Wrong format = error, nothing written.
  Bigger textures use more PS3 video memory - upscale a few, not everything.
  Untouched .dds files are left exactly as they were.

EDITING MODELS (.ff)
  1. Blender: File > Import > Wavefront (.obj), default settings.
  2. Move / scale / sculpt vertices, edit UVs. Do NOT add, delete or merge vertices
     (no remesh, no "merge by distance", no subdivide) - the game mesh is edited in place.
  3. File > Export > Wavefront (.obj) over the SAME file name, default settings.
  4. Drop the _unpacked folder on FFTool.exe. Changed models are written back;
     if a vertex count doesn't match, nothing is written and you get an error.
  Notes: skin variants (e.g. body1_2) reuse the mesh of their base model (body1_1),
  so editing the base changes all of them. Objects named surfNN_shared belong to
  another model and are ignored. Collision/bounds aren't recalculated, so keep
  shape changes moderate.

EDITING SCRIPTS (.gsc / .csc)
  Edit the files in rawfiles\ with any text editor (Notepad++, VS Code) and drop the
  _unpacked folder on FFTool.exe. WaW stores scripts as plain text and the game
  compiles them when the map loads, so what you save is exactly what the game runs.
  Size limit: a script can't grow past its original size. If yours is too big,
  FFTool automatically strips comments, indentation and blank lines (never inside
  strings) to make it fit - e.g. _zombiemode_weapons.gsc gains ~7 KB of room.
  Only if it's still too big do you get an error. Unedited scripts stay byte-identical.

Windows 10/11, 64-bit. fftool.c is the source (built against zlib 1.3.1).
