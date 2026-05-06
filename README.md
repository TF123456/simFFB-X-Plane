# simFFB-X-Plane
![simFFB](https://raw.githubusercontent.com/TF123456/simFFB-X-Plane/refs/heads/master/simffb_pic.png)

### Description
simFFB is an invaluable utility that provides joystick force feedback effects beyond those typically offered in games. simFFB was forked and X-Plane telemetry was added so that control surface position, airspeed, ground, stall warning, gear deflection and vvi are used to derive the stick forces.

See https://github.com/joeyjojojunior/simFFB for the code that this was forked from.

### To use it
Download the app from Releases (https://github.com/TF123456/simFFB-X-Plane/releases) then unzip it.

Load the app and select your joystick in both dropdowns.

For troubleshooting you can run the app with --debug to see some logs.

Check the X-Plane box when ready to connect to X-Plane.

Set:
- spring force to ~50%
- Damper force to ~30%
- Friction force to ~20%
- Shaker force to ~20%
- Bump force to ~50%

You can then fly and experience trimming forces, ground bumps, landing bumps and stick shaker.

At the bottom of the UI you can see the control surface deflections and trim status (+- 100).

You can either use the hat control of your joystick or (as the author prefers) it will respond to pressing / holding the arrow keys on your keyboard. Be sure to unbind these in X-Plane. To use the hat control you'll need to select "Progressive" or "Both" and have selected the joystick you want to use.

Also don't use trim in X-Plane as you'll be trimming the joystick into place in this app.

There is also a button to reset the trim back to zero.

This has been tested with a Microsoft SideWinder Force Feedback 2 joystick in X-Plane 12.4/2-r2 using a Cessna 172. So far the work has taken one evening, so there may be issues and improvements that need to be made.

### Build

in Powershell:

```
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' simFFB.sln /p:Configuration=Release
```
