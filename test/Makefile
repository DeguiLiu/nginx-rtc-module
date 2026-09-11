# Makefile - host unit tests for the ngx-rtc pure C protocol core.
#
# Builds the modules under src/ that carry no nginx runtime dependency, together
# with the test files in this directory. ngx_rtc_stream_module.c is the one
# exception: its entry points are static, so test_stream_module.c #includes it
# and it is compiled as part of that test object rather than as a core one.

CC      := gcc
LDLIBS  := -lssl -lcrypto

SRC_DIR   := ../src
BIN       := run_tests

# Where the nginx headers come from.
#
# MEASURED, NOT YET ADOPTED. Pointing this at a configured nginx tree compiles
# every production translation unit clean -- ngx_rtc_shm.c, ngx_rtc_core.c,
# ngx_rtc_stream_module.c and all the test files, 0 errors. What blocks it is
# not the headers but this file: nginx_stub.c does not merely declare things
# nginx declares, it IMPERSONATES nginx runtime objects. Its stub ngx_core.h
# defines ngx_slab_pool_t and ngx_shmtx_t as its own structs (malloc-backed
# slab, pthread-backed mutex) and its ngx_event_s carries fields nginx's does
# not. Against real headers those become nginx's layouts, and the host build
# would have to supply the real ngx_slab_alloc_locked / ngx_shmtx_create /
# ngx_log_error_core -- i.e. link nginx's own core, not just its types.
#
# So the hand-copied NGX_MODULE_V1 / ngx_conf_t / NGX_CONF_* block below is
# duplicated from nginx, and that is a real cost. It is not removable by
# include path alone; it goes away when the host build stops faking nginx's
# memory and event model. Windows needs those stubs regardless, since nginx's
# headers are inseparable from a Linux configure.
#
# Leave empty to use include/. A non-empty NGX_SRC must be an absolute path to
# the nginx tree containing objs/ngx_auto_config.h.
NGX_SRC ?= $(shell for d in $(CURDIR)/../../nginx-rtc-example/build/src/openresty-*/build/nginx-*; do \
                     if [ -f "$$d/objs/ngx_auto_config.h" ]; then echo "$$d"; break; fi; \
                   done)

ifneq ($(NGX_SRC),)
NGX_INC := -I$(NGX_SRC)/src/core -I$(NGX_SRC)/src/event -I$(NGX_SRC)/src/stream \
           -I$(NGX_SRC)/src/os/unix -I$(NGX_SRC)/objs
# Tells nginx_stub.c which world it is in: nginx's own struct layouts and
# macros (so it only supplies the symbols nginx's .c files would), or the
# include/ stubs (so it supplies the objects themselves).
NGX_REAL := -DNGX_RTC_REAL_NGINX_HEADERS
else
NGX_INC :=
NGX_REAL :=
endif

# Which world this run compiles in is the difference between "the tests prove
# nginx's own types and layouts" and "the tests prove include/ does", so say it
# on every run rather than leave it to be inferred from a warning line the run
# may or may not print.
#
# The probe above is relative to this directory, so a worktree that does not sit
# next to the deploy repo -- the /home/ci/... one, for instance -- comes up empty
# and lands in the stub world silently: exactly the fallback the driver scripts
# go to some trouble to avoid for their own paths. Only a caller that set
# NGX_SRC itself asked for its world; everyone else gets told which one the
# probe picked.
ifeq ($(origin NGX_SRC),command line)
$(info header world: $(if $(NGX_SRC),real ($(NGX_SRC)),stub) [NGX_SRC set by the caller])
else ifeq ($(NGX_SRC),)
$(warning header world: stub -- the NGX_SRC probe found no configured nginx tree under $(CURDIR)/../../nginx-rtc-example/build/src/openresty-*/build/nginx-*, so only the include/ stubs are compiled. Pass NGX_SRC=<abs path to an nginx tree with objs/ngx_auto_config.h> to build the real-header world.)
else
$(info header world: real ($(NGX_SRC)))
endif

# One object directory per world. make does not know that a flag changed, so a
# shared directory will happily link objects compiled against the other world's
# headers -- the silent mismatch the line above exists to expose. The caller
# may still override this (scripts/sanitize-tests.sh keys its own directory by
# sanitizer).
BUILD_DIR ?= build/$(if $(NGX_SRC),real,stub)

# -Iinclude is LAST so that, in an NGX_SRC build, the real nginx headers win the
# lookup and include/ only serves srtp2/srtp.h, which nginx does not ship.
#
# EXTRA_CPPFLAGS is the hook the driver scripts extend this with (the sanitizer
# build's third-party include path). They used to override CPPFLAGS outright,
# which silently dropped the real-header paths and built the sanitizer run
# against a different set of headers than `make test` uses.
CPPFLAGS := -D_GNU_SOURCE -I../src -I. $(EXTRA_CPPFLAGS) $(NGX_INC) -Iinclude $(NGX_REAL)

# -D_GNU_SOURCE: ngx_rtc_shm.c includes <execinfo.h> and names siginfo_t, both
# glibc extensions that need it.
#
# -DNGX_PTR_SIZE: the nginx-guarded members of ngx_rtc_session_t / _source_t
# (the queue links, the DTLS timer) exist only when it is defined. nginx's own
# ngx_auto_config.h defines it; the stub does not, so the host build passes it
# for every translation unit, which is what the real build does implicitly.
NGX_CPPFLAGS := $(CPPFLAGS) -DNGX_PTR_SIZE

# Production sources compile against nginx's own warning set, taken from nginx's
# auto/cc/gcc for the modern-GCC branch:
#
#     -O -W -Wall -Wpointer-arith -Wno-unused-parameter
#
# Two things are deliberately NOT added back:
#
#   -Wextra      nginx does not use it. It is what produced the set-but-unused
#                noise in ngx_rtc_shm.c and ngx_rtc_stream_module.c: those
#                variables are read only by ngx_log_debug*(), which nginx
#                compiles away when NGX_DEBUG is off. Suppressing them per file
#                was the old approach here; compiling with nginx's flags is the
#                honest one, because it means a warning this build reports is a
#                warning nginx would also report.
#   -Werror      nginx leaves it commented out, so a file that is correct under
#                nginx's flags can still emit one there. Turning it on here
#                would fail the host build for a reason the real build does not
#                have.
#
# Test files are not production code and keep a stricter set.
NGX_CFLAGS  := -std=c11 -O2 -W -Wall -Wpointer-arith -Wno-unused-parameter -g
TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -g

CORE_NAMES := ngx_rtc_rtp ngx_rtc_stun ngx_rtc_sdp ngx_rtc_rtcp \
              ngx_rtc_hsm ngx_rtc_session_fsm ngx_rtc_core ngx_rtc_ring \
              ngx_rtc_avsync ngx_rtc_jitter ngx_rtc_shm ngx_rtc_dtls

TEST_NAMES := ngx_rtc_test test_main test_rtp test_stun test_sdp \
              test_rtcp test_hsm test_session_fsm \
              nginx_stub test_wrap test_rtc_core test_ring test_avsync \
              test_jitter test_malformed test_shm test_stream_module

CORE_OBJS := $(addprefix $(BUILD_DIR)/core_,$(addsuffix .o,$(CORE_NAMES)))
TEST_OBJS := $(addprefix $(BUILD_DIR)/test_,$(addsuffix .o,$(TEST_NAMES)))
OBJS      := $(CORE_OBJS) $(TEST_OBJS)
DEPS      := $(OBJS:.o=.d)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(TEST_CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/core_%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(NGX_CFLAGS) $(NGX_CPPFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $(NGX_CPPFLAGS) -MMD -MP -c $< -o $@

# The one test that carries production code: it #includes ngx_rtc_stream_module.c
# so the unit's static close path and DTLS callback are callable. One
# translation unit has one flag set, and this one is production, so it is judged
# by nginx's standard. The alternative -- relaxing warnings one at a time until
# the unit compiles -- is exactly the per-file patching this Makefile no longer
# does. The test body is held to -Wall, like the unit it shares a file with.
$(BUILD_DIR)/test_test_stream_module.o: test_stream_module.c $(SRC_DIR)/ngx_rtc_stream_module.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(NGX_CFLAGS) $(NGX_CPPFLAGS) -MMD -MP -c $< -o $@

test: $(BIN)
	./$(BIN)

clean:
	rm -rf build $(BIN)

-include $(DEPS)

.PHONY: all test clean
