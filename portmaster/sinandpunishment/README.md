# Sin and Punishment - Handheld Port (NextUI / PortMaster)

Native recompiled port of *Sin and Punishment: Successor of the Earth* (*Tsumi to Batsu: Hoshi no Keishousha*) for Linux handhelds running Allwinner H700 / Rockchip RK3566 / ARM64 processors.

## In-Game Quick Settings Overlay
Press **Select** (alone) or **Menu/Guide** (or keyboard **Tab** / **F1**) at any time to open the in-game settings overlay:
- Switch Control Schemes on the fly.
- Adjust Aim Speed, Aspect Ratio, Resolution scale, and Volume.
- Quick Save & Resume or Save & Quit.
- **Select + Start** held together always saves config and exits immediately.

## Controls & Schemes
The port automatically detects menus vs. combat. In menus, the D-Pad navigates, A confirms, and B cancels. In combat, three schemes are available:

1. **RGSP: D-Pad Aim (`dpad_aim`, Default)**:
   - **D-Pad**: Smooth ramped reticle aiming.
   - **Y (West)**: Strafe Left.
   - **A (East)**: Strafe Right.
   - **R1 / R2**: Fire / Sword slash.
   - **L1**: Jump / Double jump.
   - **B (South) / L2**: 1-Button Dodge roll.
   - **X (North)**: Toggle Lock-on mode.
   - **Start**: Pause.

2. **RGSP: D-Pad Strafe (`dpad_strafe`)**:
   - **D-Pad Left / Right**: Strafe Left and Right.
   - **Face Diamond (X=Up, B=Down, Y=Left, A=Right)**: 8-Way reticle aiming.
   - **R1**: Fire / Sword slash.
   - **L1**: Jump.
   - **L2**: Dodge roll.
   - **R2**: Toggle Lock-on.

3. **Twin Stick (`twin_stick`)**:
   - **Left Stick / D-Pad L/R**: Strafe Left / Right.
   - **Right Stick**: Reticle aiming (with sensitivity & deadzone tuning).
   - **R1 / R2**: Fire.
   - **L1 / L2**: Jump.
   - **X**: Dodge roll.
   - **A**: Lock-on mode.

## ROM Setup
Place a supported N64 ROM in `roms/`:
- Japanese release (`Tsumi to Batsu - Hoshi no Keishousha`)
- English fan translation (`Sin and Punishment - Successor of the Earth`)
Supported file formats: `.z64`, `.n64`, `.v64` (~32 MiB). Endianness is automatically detected and normalized.
