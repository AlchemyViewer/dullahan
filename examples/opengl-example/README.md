### Dullahan OpenGL Example Application

Cross platform example for illustration and standalone testing of Dullahan features. Renders output to an OpenGL 2.1 quad and allows interaction using the mouse and keyboard.

Windowing, input and the OpenGL context are provided by [SDL3](https://www.libsdl.org/) and the UI is built with [Dear ImGui](https://github.com/ocornut/imgui) using its SDL3 and OpenGL2 backends. Both SDL3 and ImGui are consumed from vcpkg.

* Cross platform - builds on Windows, macOS and Linux
* Run then open Help -> About for instructions

#### Known issues:
* The requisite unique nature of the CEF cache folder means that each time this runs, a new cache folder is created and left behind. This can get quite large - should consider pruning at startup
