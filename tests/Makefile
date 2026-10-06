BUILD_DIR ?= ./build/build
# v3.4.3: fuzz/ excluded from the Catch2 suite — it is a standalone harness
# with its own main() (target: `fuzz`), never linked into tests/mock/example.
# ESP8266_SNMP: the engine sources are the vendored library (lib/SNMP_Embedded),
# not ./src (Arduino firmware). Just this tests/ dir on top.
# v3.4.3 note kept: fuzz/ was not copied into this tree at all.
SRC_DIRS ?= ../lib/SNMP_Embedded/src .
FIND_EXCLUDES := \! -name CMakeCXXCompilerId.cpp

TEST_SRCS := $(shell find $(SRC_DIRS) $(FIND_EXCLUDES) -name \*.cpp \! -name mock.cpp -print)
TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_DEPS := $(TEST_OBJS:.o=.d)

.DELETE_ON_ERROR:

CC = c++
CXX = c++

# ESP8266_SNMP: the macOS SDK path only exists on Darwin hosts (Linux CI runners
# reject -isysroot pointing at /Library/...). Guard it so darwin behavior is
# unchanged and Linux builds skip the flag.
ifeq ($(shell uname -s),Darwin)
	SYSROOT_FLAGS := -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
else
	SYSROOT_FLAGS :=
endif

CPPFLAGS ?= -I"../lib/SNMP_Embedded/src" -I"../" -I"." -MMD -MP -std=c++11 \
	$(SYSROOT_FLAGS) \
	-Wpedantic \
	-Wextra \
	-Wall \
	-Wno-error=sequence-point \
	-Wno-error=write-strings \
	-Werror \
	-DCOMPILING_TESTS	

ifdef DEBUG
	CPPFLAGS += -DDEBUG -g
endif
LDFLAGS ?= $(SYSROOT_FLAGS)

help:
	@echo "test: Make & Run tests"
	@echo "ci-test: clean first, then make & run tests (for GitHub Actions / CI runners with build-dir caching)"

$(BUILD_DIR)/test: $(TEST_OBJS)
	$(CC) $(TEST_OBJS) -o $@ $(LDFLAGS)

# c++ source
$(BUILD_DIR)/%.cpp.o: %.cpp
	$(MKDIR_P) $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
	
# inosource
$(BUILD_DIR)/%.ino.o: %.ino
	$(MKDIR_P) $(dir $@)
	cp $< $@.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I"required" -c $@.cpp -o $@
	rm $@.cpp

.PHONY: clean test benchmark ci-test test-nobuiltinuptime ci-test-nobuiltinuptime test-nozc ci-test-nozc


# v3.3.4: opt-out profile — SNMP_NO_BUILTIN_SYSUPTIME build. Separate BUILD_DIR
# so the two profiles never share stale objects. Verifies (a) clean compile with
# the flag, (b) pool/callback counts exactly one less than the default build,
# (c) sysUpTime behaves as an unregistered OID.
# NOTE on the "$(BUILD_DIR)/../src" object paths: the ../ escapes BUILD_DIR,
# so every non-default profile compiles into the SHARED ./src/ dir. Each
# profile therefore MUST clear ./src first, or it silently reuses the previous
# profile's objects (observed live: notraps linked against stale nozc objects).
# ESP8266_SNMP NOTE: upstream's profiles used BUILD_DIR=./build-nouptime
# (+ `$(RM) -r ./src`), which worked because their objects ESCAPED the build
# dir into the sibling ./src tree. Here the engine lives in lib/SNMP_Embedded:
# that escape would (a) drop .o files inside the vendored library sources and
# (b) `$(RM) -r ./src` would delete the PROJECT FIRMWARE. Profiles therefore
# nest BUILD_DIR inside ./build so escaped shared objects land in ./build/lib,
# which `clean` (rm -r ./build) already clears before each profile run.
test-nobuiltinuptime:
	$(MAKE) BUILD_DIR=./build/build-nouptime clean
	$(MAKE) BUILD_DIR=./build/build-nouptime CPPFLAGS="$(CPPFLAGS) -DSNMP_NO_BUILTIN_SYSUPTIME" test

# v3.4.0: classic-path escape-hatch profile — SNMP_ZERO_COPY=0. Separate
# BUILD_DIR. Verifies (a) the owning-container path still compiles with the
# zero-copy fast path compiled out, and (b) its suite stays green, so
# -DSNMP_ZERO_COPY=0 remains a valid field escape hatch.
test-nozc:
	$(MAKE) BUILD_DIR=./build/build-nozc clean
	$(MAKE) BUILD_DIR=./build/build-nozc CPPFLAGS="$(CPPFLAGS) -DSNMP_ZERO_COPY=0" test

ci-test-nobuiltinuptime: test-nobuiltinuptime

# v3.4.1: traps/informs compiled out — SNMP_NO_TRAPS=1. Verifies (a) the
# library compiles with the whole trap/inform path removed, (b) the derived
# pool drops the 16-slot trap-tree term (formula case), and (c) the
# remaining suite stays green.
test-notraps:
	$(MAKE) BUILD_DIR=./build/build-notraps clean
	$(MAKE) BUILD_DIR=./build/build-notraps CPPFLAGS="$(CPPFLAGS) -DSNMP_NO_TRAPS=1" test

ci-test-notraps: test-notraps

ci-test-nozc: test-nozc

clean:
	$(RM) -r ./build
	$(RM) *.d
	$(RM) *.o


test: $(BUILD_DIR)/test
	$(BUILD_DIR)/test -s

# ESP8266_SNMP: fuzz/, examples/ and the standalone mock demo are NOT
# vendored (fuzz out of scope; firmware example is src/main.cpp; mock.cpp
# is their pre-3.4.4 standalone demo and no longer compiles against the
# current engine API). Their targets went with them. The in-suite fuzz
# regression cases (F1/F2, v3.4.3) remain in tests.cpp.


ci-test: clean test

-include $(TEST_DEPS)

MKDIR_P ?= mkdir -p
