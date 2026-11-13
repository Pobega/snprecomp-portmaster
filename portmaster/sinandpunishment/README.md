# Sin and Punishment - Handheld Port (NextUI / PortMaster)

Native recompiled port of *Sin and Punishment: Successor of the Earth* (*Tsumi to Batsu: Hoshi no Keishousha*) for Linux handhelds running Allwinner H700 / Rockchip RK3566 / ARM64 processors.

## In-Game Quick Settings Overlay
Press **Select** (alone) or **Menu/Guide** (or keyboard **Tab** / **F1**) at any time to open the in-game settings overlay:
- Switch Control Schemes on the fly.
- Adjust Aim Speed, Aspect Ratio, Resolution scale, and Volume.
- Quick Save & Resume or Save & Quit.
- **Select + Start** held together always saves config and exits immediately.

## Controls & Schemes
The port automatically detects menus vs. combat. In menus, the D-Pad navigates, A confirms, and B cancels. In combat, six schemes are available:

1. **Stickless: Strafe + Shift (`dpad_strafe_shift`, Default)**:
   - **D-Pad Left / Right**: Strafe Left / Right.
   - **Hold L1 or L2 + D-Pad**: Manual Aim (with X button Re-Center).
   - **Default (no shift)**: Auto-Lock On; press **X** while moving to cycle targets.
   - **A (East) / R1 / R2**: Fire / Sword slash.
   - **B (South)**: Jump.
   - **Y (West)**: 1-Button Dodge roll.
   - **Start**: Pause.

2. **Stickless: Bumper Strafe (`bumper_strafe`)**:
   - **D-Pad**: Reticle aiming with S-Curve micro-aiming.
   - **L1 / R1**: Strafe Left / Right.
   - **A (East) / Y (West) / R2**: Fire / Sword slash.
   - **B (South)**: Jump / Double jump.
   - **X (North) / L2**: Smart Lock-On (Hold to lock, release for manual aim).
   - **Start**: Pause.

3. **Stickless: Face Strafe (`face_strafe`)**:
   - **D-Pad**: Reticle aiming with S-Curve micro-aiming.
   - **Y (West) / B (South)**: Strafe Left / Right.
   - **A (East)**: 1-Button Dodge roll (directional).
   - **R1 / R2**: Fire / Sword slash.
   - **L1 / X (North)**: Jump / Double jump.
   - **L2**: Smart Lock-On.
   - **Start**: Pause.

4. **Legacy: D-Pad Aim (`dpad_aim`)**:
   - **D-Pad**: Reticle aiming.
   - **Y (West)**: Strafe Left.
   - **A (East)**: Strafe Right.
   - **R1 / R2**: Fire / Sword slash.
   - **L1**: Jump / Double jump.
   - **B (South) / L2**: 1-Button Dodge roll.
   - **X (North)**: Lock-on.
   - **Start**: Pause.

5. **Legacy: Face Aim (`dpad_strafe`)**:
   - **D-Pad Left / Right**: Strafe Left and Right.
   - **Face Diamond (X=Up, B=Down, Y=Left, A=Right)**: Reticle aiming.
   - **R1**: Fire / Sword slash.
   - **L1**: Jump.
   - **L2**: Dodge roll.
   - **R2**: Lock-on.

6. **Twin Stick (`twin_stick`)**:
   - **Left Stick / D-Pad L/R**: Strafe Left / Right.
   - **Right Stick**: Reticle aiming.
   - **R1 / R2**: Fire.
   - **L1 / L2**: Jump.
   - **X**: Dodge roll.
   - **A**: Lock-on.

## Aiming Assists & Smart Lock-On
- **Smart Lock-On**:
  - `auto` (Default): Automatically acquires and tracks targets; holding Lock-On switches to manual aiming.
  - `hold`: Holding the Lock-On button locks onto targets; releasing restores manual crosshair aiming.
  - `off`: Classic N64 toggle mode.
- **Aim Curves**:
  - `s_curve` (Default): Frames 1-3 provide fine micro-step taps for picking off distant missiles, smoothly ramping into fast tracking via cubic Hermite curve.
  - `linear`: Direct linear acceleration.
- **Uncapped Sensitivity**: Adjustable from 50% up to 200% in 10% increments.

## ROM Setup
Place a supported N64 ROM in `roms/`:
- Japanese release (`Tsumi to Batsu - Hoshi no Keishousha`)
- English fan translation (`Sin and Punishment - Successor of the Earth`)
Supported file formats: `.z64`, `.n64`, `.v64` (~32 MiB). Endianness is automatically detected and normalized.
