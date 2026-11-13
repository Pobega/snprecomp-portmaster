# Sin and Punishment - Handheld Port (NextUI / PortMaster)

Native recompiled port of *Sin and Punishment: Successor of the Earth* (*Tsumi to Batsu: Hoshi no Keishousha*) for Linux handhelds running Allwinner H700 / Rockchip RK3566 / ARM64 processors.

## Display & Controls for RG34XX SP / RGSP
- **Resolution**: 720x480 (3:2 native screen match via `aspect_ratio = "expand"`).
- **Controls**:
  - **Left Stick / D-Pad Left/Right**: Move / strafe left and right.
  - **Right Stick**: Reticle aim (Analog Aim enabled).
  - **R1 / R2**: Fire / Sword slash.
  - **L1 / L2**: Jump.
  - **A (South)**: Lock-on / Aim mode toggle.
  - **B (East)**: Cancel.
  - **X (West)**: Dodge roll.
  - **Start**: Pause.
  - **Select + Start**: Exit game to launcher.

## ROM Setup
Place a supported N64 ROM in `roms/`:
- Japanese release (`Tsumi to Batsu - Hoshi no Keishousha`)
- English fan translation (`Sin and Punishment - Successor of the Earth`)
Supported file formats: `.z64`, `.n64`, `.v64` (~32 MiB). Endianness is automatically detected and normalized.
