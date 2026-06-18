# Pre-rendered poll pages

Fax pages served by `../pollfax.sh` for polled transfers. Each is rendered from
a source image in `../../original_images/` (scaled to the fax width; the bilevel
pages are Floyd-Steinberg dithered to 1 bit):

| file            | source             | format                         |
|-----------------|--------------------|--------------------------------|
| `standard.tiff` | `default.png`      | Group-4, 1728 wide, 204×98 dpi |
| `fine.tiff`     | `default.png`      | Group-4, 1728 wide, 204×196    |
| `superfine.tiff`| `default.png`      | Group-4, 1728 wide, 204×391    |
| `300.tiff`      | `300dpi_2.png`     | Group-4, 2592 wide, 300×300    |
| `gray.tiff`     | `graustufen_2.png` | 8-bit greyscale, 200 dpi (→ T.81 JPEG) |
| `color.tiff`    | `farbe.png`        | 24-bit RGB, 200 dpi (→ T.42 JPEG)      |

`pollfax.sh` serves these directly (no run-time rendering). Regenerate them by
running `pollfax.sh` once with `POLLFAX_RENDER=1`, or by re-running the same
`convert` recipe the script uses.
