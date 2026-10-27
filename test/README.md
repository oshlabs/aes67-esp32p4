# Tests

## Host tests (`test/host`)

Pure-C parts of the component built and run on the development host with a
plain C compiler; ESP-IDF headers are replaced by the small stubs in
`test/host/stubs`.

    make -C test/host

`sdp_test` parses descriptions modelled on this node, the aes67-linux-daemon,
a Dante device in AES67 mode and Merging SIENNA (0.333 ms, lower-case codec,
IEEE1588-2019), checks what the parser rejects and why, round-trips a
generated description through the parser, and checks the name filter.

## Minimal consumer (`test_apps/minimal_consumer`)

An ESP-IDF project that uses the component and nothing else: node, one
source, the web UI, the clock status, plus a C++ translation unit including
every public header. It must build on a clean clone; it is not a runnable
example (Ethernet bring-up is board specific, see `main/main.c` of the
example).

    cd test_apps/minimal_consumer
    idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.p4v3" set-target esp32p4
    idf.py build
