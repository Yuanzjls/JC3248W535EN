# LVGL in libdeps does not inherit env build_flags -I; CPPPATH must be set on the SCons env.
Import("env")

env.Append(CPPPATH=[env.subst("$PROJECT_DIR") + "/include"])
