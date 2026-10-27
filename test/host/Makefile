# Host-side tests for the pure-C parts of the component. Needs only a C
# compiler: ESP-IDF headers are replaced by the stubs in stubs/.
#   make -C test/host        build and run
COMP := ../../components/aes67
CFLAGS := -std=gnu17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -O1 -g \
          -I stubs -I $(COMP)/include -I $(COMP)/private_include
SRCS := sdp_test.c stubs/aes67_net_host.c $(COMP)/src/aes67_sdp.c

all: run

sdp_test: $(SRCS) $(wildcard stubs/*.h) $(wildcard $(COMP)/include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: sdp_test
	./sdp_test

clean:
	rm -f sdp_test

.PHONY: all run clean
