MAKEFLAGS += --no-builtin-variables --no-builtin-rules

.DEFAULT_GOAL := debug

# Tools.
CC ?= cc
CC := $(CC)
CLANG_FORMAT ?= clang-format
CLANG_FORMAT := $(CLANG_FORMAT)
CLANG_TIDY ?= clang-tidy
CLANG_TIDY := $(CLANG_TIDY)
CPPCHECK ?= cppcheck
CPPCHECK := $(CPPCHECK)

# Project dirs and files.
BUILD_DIR ?= build
BUILD_DIR := $(BUILD_DIR)
DIST_DIR := dist
TARGET := sosig
TSAN_TARGET := sosig-tsan
OUT ?= $(TARGET)
VERSION ?= 0.0.0-dev
GOLDEN_DEBUG_DIR := $(BUILD_DIR)/golden-debug
GOLDEN_TSAN_DIR := $(BUILD_DIR)/golden-tsan

# Every directory under `tests/fixtures` is a fixture site, and `tests/expected/<site>` holds the
# output its build must reproduce. A site added there needs no change here.
GOLDEN_SITES := $(sort $(notdir $(wildcard tests/fixtures/*)))

SRC := $(wildcard src/*/*.c)
LIB_SRC := $(filter-out src/app/main.c,$(SRC))
VENDOR_SRC := vendor/tomlc17/tomlc17.c vendor/md4c/md4c.c vendor/md4c/md4c-html.c vendor/md4c/entity.c vendor/mustache4c/mustache.c
TEST_SRC := $(wildcard tests/test_*.c)
TEST_NAMES := $(patsubst tests/test_%.c,%,$(TEST_SRC))

# Flags.
CSTD := -std=c17

# Keep a warning flag only when the active compiler accepts it, so a compiler-specific flag cannot
# break the `-Werror` build under the other compiler. The probe preprocesses an empty file, which
# accepts or rejects the flag without compiling anything.
cc-option = $(shell $(CC) -Werror $(1) -E -x c /dev/null >/dev/null 2>&1 && echo $(1))

# Warnings. `WARN_SHARED` is everything the source and the tests both get: the broad baseline
# promoted to errors, plus the specific checks that stay quiet on test code. `WARN` layers the
# stricter source-only groups on top. GCC-only and clang-only flags pass through `cc-option`, so
# each compiler enables the ones it knows and silently drops the rest.
WARN_SHARED := -Wall -Wextra -Wpedantic -Werror
# Format strings, with argument signedness where the compiler offers it.
WARN_SHARED += -Wformat=2 $(call cc-option,-Wformat-signedness)
# No variable-length arrays, so every stack buffer stays bounded by a named constant.
WARN_SHARED += -Wvla
# No goto that jumps over a variable's initialization; the cleanup idiom pre-declares instead.
WARN_SHARED += -Wjump-misses-init
# Copy-paste conditions, operator logic slips, and out-of-width shifts (GCC).
WARN_SHARED += $(call cc-option,-Wlogical-op) $(call cc-option,-Wduplicated-cond) $(call cc-option,-Wshift-overflow=2)
# Reads uninitialized on some path, out-of-range enum assignment, and comma-operator misuse (clang).
WARN_SHARED += $(call cc-option,-Wconditional-uninitialized) $(call cc-option,-Wassign-enum) $(call cc-option,-Wcomma)

# Source only. On the tests these would fire on fixture setup and deliberate casts without surfacing
# real defects, so `TEST_WARN` leaves them off.
WARN := $(WARN_SHARED)
# Implicit conversions that lose range or flip signedness.
WARN += -Wconversion -Wsign-conversion
# Declaration hygiene: full prototypes, no shadowing, no undefined macro in an `#if`.
WARN += -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wundef
# Const-correctness and read-only string literals.
WARN += -Wcast-qual -Wwrite-strings

# Test only.
TEST_WARN := $(WARN_SHARED)

CPPFLAGS := -Isrc -isystem vendor/copt -isystem vendor/tomlc17 -isystem vendor/md4c -isystem vendor/mustache4c -isystem vendor/acutest -DSOSIG_VERSION=\"$(VERSION)\"
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer
DEBUG_BASE_CFLAGS := $(CSTD) -pthread -g -Og
DEBUG_CFLAGS := $(DEBUG_BASE_CFLAGS) $(WARN) $(SAN_FLAGS)
RELEASE_CFLAGS := $(CSTD) -pthread -g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 $(WARN)
TSAN_CFLAGS := $(DEBUG_BASE_CFLAGS) $(WARN) $(TSAN_FLAGS)
TEST_CFLAGS := $(DEBUG_BASE_CFLAGS) $(TEST_WARN) $(SAN_FLAGS)
LDLIBS := -pthread

# Build artifacts.
DEBUG_DIR := $(BUILD_DIR)/debug
RELEASE_DIR := $(BUILD_DIR)/release
TSAN_DIR := $(BUILD_DIR)/tsan
TEST_DIR := $(BUILD_DIR)/test

DEBUG_BIN := $(DEBUG_DIR)/$(TARGET)
RELEASE_BIN := $(RELEASE_DIR)/$(TARGET)
TSAN_BIN := $(TSAN_DIR)/$(TSAN_TARGET)

DEBUG_SRC_OBJS := $(patsubst src/%.c,$(DEBUG_DIR)/src/%.o,$(SRC))
RELEASE_SRC_OBJS := $(patsubst src/%.c,$(RELEASE_DIR)/src/%.o,$(SRC))
TSAN_SRC_OBJS := $(patsubst src/%.c,$(TSAN_DIR)/src/%.o,$(SRC))
DEBUG_OBJS := $(DEBUG_SRC_OBJS)
RELEASE_OBJS := $(RELEASE_SRC_OBJS)
TSAN_OBJS := $(TSAN_SRC_OBJS)
DEBUG_LIB_OBJS := $(patsubst src/%.c,$(DEBUG_DIR)/src/%.o,$(LIB_SRC))
DEBUG_VENDOR_OBJS := $(patsubst vendor/%.c,$(DEBUG_DIR)/vendor/%.o,$(VENDOR_SRC))
RELEASE_VENDOR_OBJS := $(patsubst vendor/%.c,$(RELEASE_DIR)/vendor/%.o,$(VENDOR_SRC))
TSAN_VENDOR_OBJS := $(patsubst vendor/%.c,$(TSAN_DIR)/vendor/%.o,$(VENDOR_SRC))
TEST_OBJS := $(patsubst tests/%.c,$(TEST_DIR)/tests/%.o,$(TEST_SRC))
TEST_BINS := $(addprefix $(TEST_DIR)/test_,$(TEST_NAMES))

# `test_template` links its own `template.o` built with a small rendered-output bound. That limit is
# a byte count. To reach it, the test must accumulate that many bytes. An assertion at the
# production value would cost too much memory for one diagnostic.
TEMPLATE_TEST_OUTPUT_LEN_MAX := 65536
TEMPLATE_TEST_OBJ := $(TEST_DIR)/src/build/template.o
TEMPLATE_TEST_LIB_OBJS := $(filter-out $(DEBUG_DIR)/src/build/template.o,$(DEBUG_LIB_OBJS)) \
	$(TEMPLATE_TEST_OBJ)

DEPS := $(DEBUG_OBJS:.o=.d) $(RELEASE_OBJS:.o=.d) $(TSAN_OBJS:.o=.d) \
	$(DEBUG_VENDOR_OBJS:.o=.d) $(RELEASE_VENDOR_OBJS:.o=.d) $(TSAN_VENDOR_OBJS:.o=.d) \
	$(TEST_OBJS:.o=.d) $(TEMPLATE_TEST_OBJ:.o=.d)

ALL_OBJS := $(DEBUG_OBJS) $(RELEASE_OBJS) $(TSAN_OBJS) \
	$(DEBUG_VENDOR_OBJS) $(RELEASE_VENDOR_OBJS) $(TSAN_VENDOR_OBJS) \
	$(TEST_OBJS) $(TEMPLATE_TEST_OBJ)
ALL_BINS := $(DEBUG_BIN) $(RELEASE_BIN) $(TSAN_BIN) $(TEST_BINS)
BUILD_DIRS := $(sort $(dir $(ALL_OBJS) $(ALL_BINS)))

.PHONY: all debug release tsan dirs format lint test-debug golden-debug golden-tsan ci clean

# Public targets.
all: debug

debug: $(DEBUG_BIN)
	@cp "$<" "$(TARGET)"

release: $(RELEASE_BIN)
	@mkdir -p "$(dir $(OUT))"
	@cp "$<" "$(OUT)"

tsan: $(TSAN_BIN)
	@cp "$<" "$(TSAN_TARGET)"

dirs:
	@mkdir -p $(BUILD_DIRS)

# Canned recipes shared by every build variant.
define COMPILE
@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"
endef

define COMPILE_VENDOR
@$(CC) $(CPPFLAGS) $(CFLAGS) -fno-sanitize=null,object-size -w -MMD -MP -c "$<" -o "$@"
endef

define LINK
@$(CC) $(CFLAGS) $^ $(LDLIBS) -o "$@"
endef

# Generate the compile and link rules for one build variant.
# $(1) is the variable prefix: DEBUG, RELEASE, or TSAN.
define build_variant
$$($(1)_OBJS): CFLAGS := $$($(1)_CFLAGS)
$$($(1)_SRC_OBJS): $$($(1)_DIR)/src/%.o: src/%.c Makefile | dirs
	$$(COMPILE)

$$($(1)_VENDOR_OBJS): CFLAGS := $$($(1)_CFLAGS)
$$($(1)_VENDOR_OBJS): $$($(1)_DIR)/vendor/%.o: vendor/%.c Makefile | dirs
	$$(COMPILE_VENDOR)

$$($(1)_BIN): CFLAGS := $$($(1)_CFLAGS)
$$($(1)_BIN): $$($(1)_OBJS) $$($(1)_VENDOR_OBJS) | dirs
	$$(LINK)
endef

$(eval $(call build_variant,DEBUG))
$(eval $(call build_variant,RELEASE))
$(eval $(call build_variant,TSAN))

# Tests reuse the debug library and vendor objects.
$(TEST_OBJS): CFLAGS := $(TEST_CFLAGS)
$(TEST_OBJS): $(TEST_DIR)/tests/%.o: tests/%.c Makefile | dirs
	$(COMPILE)

$(TEST_DIR)/test_%: $(TEST_DIR)/tests/test_%.o $(DEBUG_LIB_OBJS) $(DEBUG_VENDOR_OBJS) Makefile | dirs
	@$(CC) $(TEST_CFLAGS) "$<" $(DEBUG_LIB_OBJS) $(DEBUG_VENDOR_OBJS) $(LDLIBS) -o "$@"

# The template test and the `template.o` it links share the small bound, so the test derives the
# expected diagnostic from the same value the code enforces. The build compiles it with the debug
# source flags, not the laxer test flags, so `src/` warnings still apply to it.
$(TEST_DIR)/tests/test_template.o $(TEMPLATE_TEST_OBJ): \
	CPPFLAGS += -DSOSIG_RENDER_OUTPUT_LEN_MAX=$(TEMPLATE_TEST_OUTPUT_LEN_MAX)

$(TEMPLATE_TEST_OBJ): CFLAGS := $(DEBUG_CFLAGS)
$(TEMPLATE_TEST_OBJ): src/build/template.c Makefile | dirs
	$(COMPILE)

$(TEST_DIR)/test_template: $(TEST_DIR)/tests/test_template.o $(TEMPLATE_TEST_LIB_OBJS) \
		$(DEBUG_VENDOR_OBJS) Makefile | dirs
	@$(CC) $(TEST_CFLAGS) "$<" $(TEMPLATE_TEST_LIB_OBJS) $(DEBUG_VENDOR_OBJS) $(LDLIBS) -o "$@"

# Quality
format:
	@$(CLANG_FORMAT) -i src/*/*.[ch] tests/*.c

lint:
	@if command -v $(CLANG_FORMAT) >/dev/null 2>&1; then \
		$(CLANG_FORMAT) --dry-run --Werror src/*/*.[ch] tests/*.c; \
	else \
		echo "skipping clang-format: $(CLANG_FORMAT) not found"; \
	fi
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		$(CLANG_TIDY) --quiet src/*/*.c -- $(CPPFLAGS) -std=c17; \
	else \
		echo "skipping clang-tidy: $(CLANG_TIDY) not found"; \
	fi
	@if command -v $(CPPCHECK) >/dev/null 2>&1; then \
		$(CPPCHECK) --enable=warning,performance,portability --std=c17 --quiet --error-exitcode=1 src tests; \
	else \
		echo "skipping cppcheck: $(CPPCHECK) not found"; \
	fi

test-debug: debug $(TEST_BINS)
	@set -e; for test in $(TEST_BINS); do $$test; done

# Builds every fixture site and diffs each result against its expected output. The scratch root is
# wiped first, so a site left behind by an earlier run cannot pass. The binary runs with the scratch
# site as its working directory, which is what makes the relative dirs in `sosig.toml` resolve
# inside the scratch tree, so it has to be named by an absolute path. $(1) is the scratch root, $(2)
# the binary, and $(3) extra `build` flags.
define GOLDEN_DIFF
@rm -rf "$(1)"
@set -e; for site in $(GOLDEN_SITES); do \
	mkdir -p "$(1)/$$site"; \
	cp -R "tests/fixtures/$$site/." "$(1)/$$site/"; \
	(cd "$(1)/$$site" && "$(2)" build $(3)); \
	diff -ru "tests/expected/$$site" "$(1)/$$site/public"; \
done
endef

golden-debug: debug
	$(call GOLDEN_DIFF,$(GOLDEN_DEBUG_DIR),$(abspath $(DEBUG_BIN)),)

golden-tsan: tsan
# This target is verbose on purpose. The progress dots are the only hand-written locking outside the
# pool. This is the one target that runs under `ThreadSanitizer`.
	$(call GOLDEN_DIFF,$(GOLDEN_TSAN_DIR),$(abspath $(TSAN_BIN)),-v)

ci:
	@$(MAKE) lint
	@$(MAKE) release
	@$(MAKE) test-debug
	@$(MAKE) golden-debug
	@$(MAKE) golden-tsan

clean:
	@rm -rf "$(BUILD_DIR)" "$(DIST_DIR)" "$(TARGET)" "$(TSAN_TARGET)"

# Generated dependencies
-include $(DEPS)
