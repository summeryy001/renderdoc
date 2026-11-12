# SPIR-V open source plugins build

These scripts are used to build the latest versions of glslang, SPIRV-Cross and SPIRV-Tools for distribution as plugins. On windows this is just a regular build really, on linux it will build in the newman docker with static linking, to ensure maximum compatibility.

**Most users don't need to build this.**. The pre-built latest plugins packages are provided online which are included in every build, that you can download yourself:

* Windows: https://newman.org/plugins.zip
* Linux: https://newman.org/plugins.tgz

These scripts are just here so that a NewMan developer can update the plugins when needed.
