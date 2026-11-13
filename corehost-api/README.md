# :corehost-api (MIT)

The generic binder protocol between the RetroNest app and its core host process:

- `ICoreHost.aidl` / `ICoreHostCallback.aidl` (package `com.retronest.corehost`)
- `CoreHostProtocol` (Bundle keys, state / capability constants, host service class name)
- `RetroPadButtons` (the libretro RetroPad bit mask used by `setButtons`)

Protocol only: no emulator code and no app code. The closed app compiles against this module and talks
to `:corehost` (GPL-2.0-or-later) at run time through the binder, nothing else.
