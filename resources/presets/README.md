# Pipeline preset images

Drop image files (PNG, DDS, BLP, TGA, …) here. The Pipelines **Resource** input
node picks from this folder; the node stores a path **relative to this folder**.

This directory is copied next to the executable on build (CMake target
`WhiteoutTexAssets`) so the picker and runtime can find it via
`SDL_GetBasePath()/presets`.
