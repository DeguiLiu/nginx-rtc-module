# Makefile - host unit tests for the ngx-rtc pure C protocol core.
#
# Builds the nginx-free modules under src/ together with the test files in this
# directory. STUN links -lcrypto for HMAC-SHA1; the libsrtp2 dependency of
# ngx_rtc_srtp.c is stubbed out (test/include/srtp2/srtp.h) because this round
# tests only rtp / stun / sdp / rtcp / hsm / session_fsm.

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g -MMD -MP
CPPFLAGS := -I../src -I. -Iinclude
LDLIBS  := -lcrypto

SRC_DIR   := ../src
BUILD_DIR := build
BIN       := run_tests

CORE_NAMES := ngx_rtc_rtp ngx_rtc_stun ngx_rtc_sdp ngx_rtc_rtcp \
              ngx_rtc_hsm ngx_rtc_session_fsm ngx_rtc_core ngx_rtc_ring

TEST_NAMES := ngx_rtc_test test_main test_rtp test_stun test_sdp \
              test_rtcp test_hsm test_session_fsm \
              nginx_stub test_wrap test_rtc_core test_ring test_avsync \
              test_malformed

CORE_OBJS := $(addprefix $(BUILD_DIR)/core_,$(addsuffix .o,$(CORE_NAMES)))
TEST_OBJS := $(addprefix $(BUILD_DIR)/test_,$(addsuffix .o,$(TEST_NAMES)))
OBJS      := $(CORE_OBJS) $(TEST_OBJS)
DEPS      := $(OBJS:.o=.d)

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/core_%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ngx_rtc_core.c uses the nginx rbtree/queue types guarded by NGX_PTR_SIZE; the
# host tests compile it with the test/include nginx stubs so the GOP-ring
# helpers (ngx_rtc_rtp_ring_push/_get/_replay) can run without nginx. The
# registry half of the file is never exercised by host tests (see nginx_stub.c).
$(BUILD_DIR)/core_ngx_rtc_core.o: $(SRC_DIR)/ngx_rtc_core.c \
                                  include/ngx_config.h include/ngx_core.h include/ngx_event.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DNGX_PTR_SIZE -c $< -o $@

$(BUILD_DIR)/test_%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD_DIR) $(BIN)

-include $(DEPS)

.PHONY: all test clean
