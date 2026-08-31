# Stub ESP-IDF headers

Just enough of ESP-IDF for a host compiler to parse the firmware sources.

`pio/src/*.c` and `port/esp32s3/*.c` are the quarter of the project no host
suite compiles, and that is where every defect the audit found lived. A missing
forward declaration in `gate_app.c` got as far as an ESP-IDF build with
`-Werror` before anyone noticed -- a compiler had never looked at the file.

These stubs do not emulate anything and are not linked. They exist so
`make lint` can run a **syntax and type check** over those files in a second,
catching implicit declarations, conflicting prototypes, wrong argument counts
and typos -- the errors that are pure compiler work and need no hardware.

They deliberately do not try to be faithful. Anything behavioural still needs
the real toolchain.
