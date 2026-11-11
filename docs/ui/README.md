# Device UI

The native interface uses a 480 × 480 display. Each workspace gives its main
controls most of the screen. Menu opens mode selection, Status and Settings.
The physical AI button also opens Menu with a double press.

| Workspace | Design and behavior |
| --- | --- |
| [Mouse](mouse.png) | The pad occupies the center, with a separate scroll column and explicit click and drag controls. Calibrated aiming pauses during touch. |
| [Keyboard](keyboard.png) | The draft stays visible above the character grid. Confirm characters locally, then explicitly Send. Keys opens direct host actions without changing the draft. |
| [Audio](audio.png) | Mic and Speaker show independent route readiness and job state. Choosing USB or Bluetooth does not start either job. |

The renders come from the [native LVGL suite](../../tests/ui/README.md), using
the firmware UI and synthetic state. They illustrate the display layout and
do not establish device or host behavior. The [interaction contract](../interaction.md)
defines transitions, controls and safety boundaries. The
[user guide](../user-guide.md) covers operation on the device.
