## Makefile
## Summary: Cross-compiles kaiman with its source dependencies.
##
## Author:  KaisarCode
## Website: https://kaisarcode.com
## License: https://www.gnu.org/licenses/lgpl-3.0.html

BIN_DIR := bin
BUILD_DIR := .build
WINE ?= wine
WINE_X86_64_CC ?= x86_64-w64-mingw32-gcc
WINE_X86_64_CXX ?= x86_64-w64-mingw32-g++
KC_VERSION := $(shell sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' manifest.json)
ifeq ($(strip $(KC_VERSION)),)
$(error Missing version in manifest.json)
endif
OSXCROSS_ROOT ?= $(HOME)/.local/share/osxcross/target
OSXCROSS_X86_64_AR := $(shell ls "$(OSXCROSS_ROOT)"/bin/x86_64-apple-darwin*-ar 2>/dev/null | sort -V | tail -n 1)
OSXCROSS_AARCH64_AR := $(shell ls "$(OSXCROSS_ROOT)"/bin/aarch64-apple-darwin*-ar 2>/dev/null | sort -V | tail -n 1)
IOS_DEPLOYMENT_TARGET ?= 13.0
IPHONEOS_SDK ?= $(shell ls -d "$(OSXCROSS_ROOT)"/SDK/iPhoneOS*.sdk 2>/dev/null | sort -V | tail -n 1)
IPHONESIMULATOR_SDK ?= $(shell ls -d "$(OSXCROSS_ROOT)"/SDK/iPhoneSimulator*.sdk 2>/dev/null | sort -V | tail -n 1)
OSXCROSS_IOS_AARCH64_CC := $(OSXCROSS_ROOT)/bin/ios64-clang
OSXCROSS_IOSSIM_AARCH64_CC := $(OSXCROSS_ROOT)/bin/iossim64-clang
OSXCROSS_IOSSIM_X86_64_CC := $(OSXCROSS_ROOT)/bin/iossimx64-clang

ANDROID_HOME ?= $(HOME)/.local/share/android-sdk
NDK_VERSION ?= 27.2.12479018
NDK_BIN ?= $(ANDROID_HOME)/ndk/$(NDK_VERSION)/toolchains/llvm/prebuilt/linux-x86_64/bin
ANDROID_API ?= 21

NATIVE_ARCH := $(shell uname -m)
NATIVE_SYSTEM := $(shell uname -s)
ifeq ($(NATIVE_ARCH),amd64)
NATIVE_ARCH := x86_64
endif
ifeq ($(NATIVE_ARCH),arm64)
NATIVE_ARCH := aarch64
endif
ifeq ($(NATIVE_ARCH),armv7l)
NATIVE_ARCH := armv7
endif
NATIVE_PLATFORM := linux
NATIVE_EXT := so
ifeq ($(NATIVE_SYSTEM),Darwin)
NATIVE_PLATFORM := macos
NATIVE_EXT := dylib
endif
ifneq ($(filter MINGW% MSYS% CYGWIN%,$(NATIVE_SYSTEM)),)
NATIVE_PLATFORM := windows
NATIVE_EXT := dll
endif
NATIVE_TARGET := $(NATIVE_PLATFORM)/$(NATIVE_ARCH)

LINUX_TARGETS := linux/x86_64 linux/i686 linux/aarch64 linux/armv7 \
    linux/armv7hf linux/riscv64 linux/powerpc64le linux/mips \
    linux/mipsel linux/mips64el linux/s390x linux/loongarch64
WINDOWS_TARGETS := windows/x86_64 windows/i686
MACOS_TARGETS := macos/x86_64 macos/aarch64
IOS_TARGETS := ios/aarch64 iossim/aarch64 iossim/x86_64
DARWIN_SYSTEM_LIBS := -Wl,-framework,Security -Wl,-framework,CoreFoundation -lm
ANDROID_TARGETS := android/aarch64 android/armv7
SUPPORTED_TARGETS := $(LINUX_TARGETS) $(WINDOWS_TARGETS) \
    $(MACOS_TARGETS) $(IOS_TARGETS) $(ANDROID_TARGETS)

.DEFAULT_GOAL := $(NATIVE_TARGET)
.PHONY: all test wine clean build-target check-deps submodules deps $(SUPPORTED_TARGETS)

all: $(SUPPORTED_TARGETS)

define linux_target
linux/$(1):
	@$(MAKE) -s --no-print-directory build-target TARGET=linux/$(1) \
		TARGET_CC="$(2)" TARGET_EXT=$(NATIVE_EXT) TARGET_CFLAGS="-fPIC" \
		TARGET_SHARED="-shared" \
		TARGET_SYSTEM_LIBS="-pthread -ldl -lm $(if $(filter armv7 armv7hf mips mipsel mips64el,$(1)),-latomic)"
endef

$(eval $(call linux_target,x86_64,gcc))
$(eval $(call linux_target,i686,i686-linux-gnu-gcc))
$(eval $(call linux_target,aarch64,aarch64-linux-gnu-gcc))
$(eval $(call linux_target,armv7,arm-linux-gnueabi-gcc))
$(eval $(call linux_target,armv7hf,arm-linux-gnueabihf-gcc))
$(eval $(call linux_target,riscv64,riscv64-linux-gnu-gcc))
$(eval $(call linux_target,powerpc64le,powerpc64le-linux-gnu-gcc))
$(eval $(call linux_target,mips,mips-linux-gnu-gcc))
$(eval $(call linux_target,mipsel,mipsel-linux-gnu-gcc-13))
$(eval $(call linux_target,mips64el,mips64el-linux-gnuabi64-gcc))
$(eval $(call linux_target,s390x,s390x-linux-gnu-gcc))
$(eval $(call linux_target,loongarch64,loongarch64-linux-gnu-gcc))

define windows_target
windows/$(1):
	@$(MAKE) -s --no-print-directory build-target TARGET=windows/$(1) \
		TARGET_CC="$(2)" TARGET_EXT=dll TARGET_CFLAGS="" \
		TARGET_CLI_EXT=.exe TARGET_SHARED="" \
		TARGET_SYSTEM_LIBS="-lws2_32 -lcrypt32 -lbcrypt -lm"
endef

$(eval $(call windows_target,x86_64,x86_64-w64-mingw32-gcc))
$(eval $(call windows_target,i686,i686-w64-mingw32-gcc))

define macos_target
macos/$(1):
	@$(MAKE) -s --no-print-directory build-target TARGET=macos/$(1) \
		TARGET_CC="$(OSXCROSS_ROOT)/bin/$(2)" TARGET_AR="$(3)" \
		TARGET_EXT=dylib TARGET_CFLAGS="-fPIC" TARGET_SYSTEM_LIBS="$(DARWIN_SYSTEM_LIBS)" \
		TARGET_SHARED="-dynamiclib -Wl,-install_name,@rpath/libkaiman.dylib"
endef

$(eval $(call macos_target,x86_64,o64-clang,$(OSXCROSS_X86_64_AR)))
$(eval $(call macos_target,aarch64,oa64-clang,$(OSXCROSS_AARCH64_AR)))

define ios_target
$(1)/$(2):
	@test -d "$(5)" || { echo "kaiman: missing iOS SDK: $(5)" >&2; exit 1; }
	@$(MAKE) -s --no-print-directory build-target TARGET=$(1)/$(2) \
		TARGET_CC="$(3)" TARGET_AR="$(4)" TARGET_EXT=dylib TARGET_LIBRARY_ONLY=1 \
		TARGET_CFLAGS="-fPIC -isysroot $(5) $(6) -Wno-unused-command-line-argument" \
		TARGET_SDK="$(5)" TARGET_SYSTEM_LIBS="$(DARWIN_SYSTEM_LIBS)"
endef

$(eval $(call ios_target,ios,aarch64,$(OSXCROSS_IOS_AARCH64_CC),$(OSXCROSS_AARCH64_AR),$(IPHONEOS_SDK),-miphoneos-version-min=$(IOS_DEPLOYMENT_TARGET)))
$(eval $(call ios_target,iossim,aarch64,$(OSXCROSS_IOSSIM_AARCH64_CC),$(OSXCROSS_AARCH64_AR),$(IPHONESIMULATOR_SDK),-mios-simulator-version-min=$(IOS_DEPLOYMENT_TARGET)))
$(eval $(call ios_target,iossim,x86_64,$(OSXCROSS_IOSSIM_X86_64_CC),$(OSXCROSS_X86_64_AR),$(IPHONESIMULATOR_SDK),-mios-simulator-version-min=$(IOS_DEPLOYMENT_TARGET)))

define android_target
android/$(1):
	@$(MAKE) -s --no-print-directory build-target TARGET=android/$(1) \
		TARGET_CC="$(NDK_BIN)/$(2)$(ANDROID_API)-clang" \
		TARGET_EXT=$(NATIVE_EXT) TARGET_CFLAGS="-fPIC" TARGET_SHARED="-shared" \
		TARGET_SYSTEM_LIBS="-ldl -lm"
endef

$(eval $(call android_target,aarch64,aarch64-linux-android))
$(eval $(call android_target,armv7,armv7a-linux-androideabi))

DEP_ROOT := lib/kaisarcode
DEP_NAMES := http netl tls
TLS_SHARED_NAME := libtls.so
TLS_IMPORT_NAME :=
ifeq ($(findstring windows/,$(TARGET)),windows/)
TLS_SHARED_NAME := libtls.dll
TLS_IMPORT_NAME := libtls.dll.a
else ifneq ($(filter macos/% ios/% iossim/%,$(TARGET)),)
TLS_SHARED_NAME := libtls.dylib
endif
DEP_PATHS := $(foreach dep,$(DEP_NAMES),$(DEP_ROOT)/lib$(dep).c)
DEP_LIB_DIR = $(BUILD_DIR)/deps/$(TARGET)
DEP_LIBS = $(foreach dep,$(DEP_NAMES),$(DEP_LIB_DIR)/lib$(dep).a)
DEP_HEADERS := $(foreach dep,$(DEP_NAMES),$(DEP_ROOT)/lib$(dep).c/src/lib$(dep).h)
DEP_INCLUDES := $(foreach dep,$(DEP_NAMES),-I$(DEP_ROOT)/lib$(dep).c/src)
DEP_LINKS = $(DEP_LIBS) $(TARGET_SYSTEM_LIBS)

submodules:
	@for path in $(DEP_PATHS); do \
		test -f "$$path/Makefile" || { echo "kaiman: missing dependency $$path" >&2; exit 1; }; \
	done

deps: submodules
	@mkdir -p "$(DEP_LIB_DIR)"
	@log=$$(mktemp) || exit 1; \
	for name in $(DEP_NAMES); do \
		if ! $(MAKE) -s --no-print-directory -C "$(DEP_ROOT)/lib$$name.c" "$(TARGET)" > "$$log" 2>&1; then \
			echo "kaiman: dependency lib$$name failed for $(TARGET)" >&2; \
			cat "$$log" >&2; \
			rm -f "$$log"; \
			exit 1; \
		fi; \
		source="$(DEP_ROOT)/lib$$name.c/bin/$(TARGET)/lib$$name.a"; \
		target="$(DEP_LIB_DIR)/lib$$name.a"; \
		test -f "$$source" || { echo "kaiman: missing dependency for $(TARGET): $$source" >&2; rm -f "$$log"; exit 1; }; \
		if ! cmp -s "$$source" "$$target"; then \
			cp "$$source" "$$target" || { rm -f "$$log"; exit 1; }; \
		fi; \
		if [ "$$name" = "tls" ] && [ -z "$(TARGET_LIBRARY_ONLY)" ]; then \
			shared="$(DEP_ROOT)/libtls.c/bin/$(TARGET)/$(TLS_SHARED_NAME)"; \
			copied="$(DEP_LIB_DIR)/$(TLS_SHARED_NAME)"; \
			test -f "$$shared" || { echo "kaiman: missing dependency for $(TARGET): $$shared" >&2; rm -f "$$log"; exit 1; }; \
			if ! cmp -s "$$shared" "$$copied"; then \
				cp "$$shared" "$$copied" || { rm -f "$$log"; exit 1; }; \
			fi; \
			mkdir -p "$(BIN_DIR)/$(TARGET)" || { rm -f "$$log"; exit 1; }; \
			cp "$$shared" "$(BIN_DIR)/$(TARGET)/$(TLS_SHARED_NAME)" || { rm -f "$$log"; exit 1; }; \
			if [ -n "$(TLS_IMPORT_NAME)" ]; then \
				import="$(DEP_ROOT)/libtls.c/bin/$(TARGET)/$(TLS_IMPORT_NAME)"; \
				import_copy="$(DEP_LIB_DIR)/$(TLS_IMPORT_NAME)"; \
				test -f "$$import" || { echo "kaiman: missing dependency for $(TARGET): $$import" >&2; rm -f "$$log"; exit 1; }; \
				if ! cmp -s "$$import" "$$import_copy"; then \
					cp "$$import" "$$import_copy" || { rm -f "$$log"; exit 1; }; \
				fi; \
			fi; \
		fi; \
	done; \
	rm -f "$$log"

OUT_DIR = $(BIN_DIR)/$(TARGET)
OUT_SHARED = $(OUT_DIR)/libkaiman.$(TARGET_EXT)
OUT_STATIC = $(OUT_DIR)/libkaiman.a
OUT_CLI = $(OUT_DIR)/kaiman$(TARGET_CLI_EXT)
OUT_OBJECT = $(BUILD_DIR)/$(TARGET)/libkaiman.o
BUILD_TARGET_DIR = $(BUILD_DIR)/$(TARGET)$(if $(TARGET_AR),-$(notdir $(TARGET_AR)),)
TARGET_PRODUCT = $(if $(filter ios/% iossim/%,$(TARGET)),$(OUT_STATIC),$(OUT_CLI))

check-deps: deps
	@command -v "$(TARGET_CC)" >/dev/null 2>&1 || { echo "kaiman: missing target compiler $(TARGET_CC)" >&2; exit 1; }
	@if [ -n "$(TARGET_AR)" ]; then \
		command -v "$(TARGET_AR)" >/dev/null 2>&1 || \
		{ echo "kaiman: missing target archiver $(TARGET_AR)" >&2; exit 1; }; \
	fi
	@for path in $(DEP_LIBS) $(DEP_HEADERS); do \
		test -f "$$path" || { echo "kaiman: missing dependency for $(TARGET): $$path" >&2; exit 1; }; \
	done

build-target: check-deps
	@echo "==> kaiman $(TARGET)"
	@mkdir -p "$(OUT_DIR)"
	@case "$(TARGET)" in \
		linux/*) system=Linux ;; \
		windows/*) system=Windows ;; \
		macos/*) system=Darwin ;; \
		ios/*|iossim/*) system=iOS ;; \
		android/*) system=Android ;; \
		*) echo "Unsupported target: $(TARGET)" >&2; exit 1 ;; \
	esac; \
	extra=""; \
	library_only=""; \
	case "$(TARGET)" in \
		android/*) \
			case "$(TARGET)" in \
				android/aarch64) abi=arm64-v8a ;; \
				android/armv7) abi=armeabi-v7a ;; \
			esac; \
			extra="-DCMAKE_TOOLCHAIN_FILE=$(ANDROID_HOME)/ndk/$(NDK_VERSION)/build/cmake/android.toolchain.cmake -DANDROID_ABI=$$abi -DANDROID_PLATFORM=android-21" ;; \
		ios/*|iossim/*) \
			extra="-DCMAKE_OSX_SYSROOT=$(TARGET_SDK) -DCMAKE_OSX_DEPLOYMENT_TARGET=$(IOS_DEPLOYMENT_TARGET)"; \
			library_only="-DKC_LIBRARY_ONLY=1" ;; \
	esac; \
	case "$(TARGET)" in \
		macos/*|ios/*|iossim/*) \
			PATH="$(OSXCROSS_ROOT)/bin:$$PATH"; \
			LD_LIBRARY_PATH="$(OSXCROSS_ROOT)/lib:$${LD_LIBRARY_PATH:-}"; \
			export PATH LD_LIBRARY_PATH ;; \
	esac; \
	compiler_args="-DCMAKE_C_COMPILER=$(TARGET_CC)"; \
	if [ "$$system" = Android ]; then compiler_args=""; fi; \
	ar_args=""; \
	if [ -n "$(TARGET_AR)" ]; then \
		ar_args="-DCMAKE_AR=$$(command -v "$(TARGET_AR)")"; \
	fi; \
	mkdir -p "$(BUILD_TARGET_DIR)" || exit 1; \
	configure_errors="$(BUILD_TARGET_DIR)/configure.stderr"; \
	configure() { \
		cmake -S . -B "$(BUILD_TARGET_DIR)" -G Ninja -Wno-dev \
			--no-warn-unused-cli \
			-DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME="$$system" \
			$$compiler_args $$ar_args \
			-DCMAKE_C_FLAGS="$(TARGET_CFLAGS)" \
			-DKC_TARGET="$(TARGET)" \
			$$library_only \
			-DKC_SHARED_FLAGS="$(TARGET_SHARED)" \
			-DKC_SYSTEM_LIBS="$(TARGET_SYSTEM_LIBS)" \
			$$extra > /dev/null 2> "$$configure_errors"; \
	}; \
	configure; \
	configure_status=$$?; \
	if [ "$$configure_status" -ne 0 ]; then \
		cat "$$configure_errors" >&2; \
		exit "$$configure_status"; \
	fi; \
	cat "$$configure_errors" >&2; \
	rm -f "$$configure_errors"; \
	build_log="$$(mktemp)"; \
	cmake --build "$(BUILD_TARGET_DIR)" 2>&1 > "$$build_log"; \
	build_status=$$?; \
	if [ "$$build_status" -ne 0 ]; then cat "$$build_log" >&2; fi; \
	rm -f "$$build_log"; \
	exit "$$build_status"
	@echo "OK kaiman $(TARGET)"

NATIVE_SHARED := $(BIN_DIR)/$(NATIVE_TARGET)/libkaiman.$(NATIVE_EXT)

$(BUILD_DIR)/test/kaiman-test: src/test.c src/libkaiman.h $(NATIVE_SHARED)
	@mkdir -p "$(BUILD_DIR)/test"
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc $(DEP_INCLUDES) \
		-o "$@" src/test.c -L"$(BIN_DIR)/$(NATIVE_TARGET)" -lkaiman

test:
	@if [ -n "$(filter wine,$(MAKECMDGOALS))" ]; then \
		if ! command -v "$(WINE)" >/dev/null 2>&1; then \
			echo "Missing Wine runtime: $(WINE)" >&2; exit 1; \
		fi; \
		if ! command -v "$(WINE_X86_64_CC)" >/dev/null 2>&1; then \
			echo "Missing Windows cross-compiler" >&2; exit 1; \
		fi; \
		for file in libkaiman.dll kaiman.exe; do \
			if [ ! -f "$(CURDIR)/$(BIN_DIR)/windows/x86_64/$$file" ]; then \
				echo "Missing Windows artifact: $$file; run make windows/x86_64 first" >&2; \
				exit 1; \
			fi; \
		done; \
		mkdir -p "$(BUILD_DIR)/test-wine" || exit 1; \
		"$(WINE_X86_64_CC)" -std=c11 -O2 -Wall -Wextra -Werror -Isrc \
			$(DEP_INCLUDES) -o "$(BUILD_DIR)/test-wine/kaiman-test.exe" src/test.c \
			-L"$(BIN_DIR)/windows/x86_64" -lkaiman -lws2_32 || exit 1; \
		cp "$(BIN_DIR)/windows/x86_64/libkaiman.dll" \
			"$(BIN_DIR)/windows/x86_64/libtls.dll" \
			"$(BIN_DIR)/windows/x86_64/kaiman.exe" \
			"$(BUILD_DIR)/test-wine/" || exit 1; \
		for dll in \
			"$$($(WINE_X86_64_CC) -print-file-name=libgcc_s_seh-1.dll)" \
			"$$($(WINE_X86_64_CC) -print-file-name=libstdc++-6.dll)" \
			"$$($(WINE_X86_64_CXX) -print-file-name=libwinpthread-1.dll)"; do \
			if [ -f "$$dll" ]; then \
				cp "$$dll" "$(BUILD_DIR)/test-wine/" || exit 1; \
			fi; \
		done; \
		(cd "$(BUILD_DIR)/test-wine" && \
			EGL_LOG_LEVEL=fatal WINEDEBUG=-all "$(WINE)" kaiman-test.exe config.ini); \
	else \
		if [ ! -f "$(CURDIR)/$(NATIVE_SHARED)" ] || \
			[ ! -f "$(CURDIR)/$(BIN_DIR)/$(NATIVE_TARGET)/kaiman$(TARGET_CLI_EXT)" ]; then \
			echo "Missing native library or CLI: run make first" >&2; \
			exit 1; \
		fi; \
		$(MAKE) -s --no-print-directory "$(BUILD_DIR)/test/kaiman-test" || exit 1; \
		cp "$(BIN_DIR)/$(NATIVE_TARGET)/kaiman$(TARGET_CLI_EXT)" \
			"$(BUILD_DIR)/test/" || exit 1; \
		(cd "$(BUILD_DIR)/test" && \
			LD_LIBRARY_PATH="$(CURDIR)/$(BIN_DIR)/$(NATIVE_TARGET):$$LD_LIBRARY_PATH" \
			DYLD_LIBRARY_PATH="$(CURDIR)/$(BIN_DIR)/$(NATIVE_TARGET):$$DYLD_LIBRARY_PATH" \
			./kaiman-test config.ini || exit 1); \
	fi

wine:
	@if [ -z "$(filter test,$(MAKECMDGOALS))" ]; then \
		echo "Use 'make test wine' to run tests through Wine." >&2; \
		exit 1; \
	fi
	@:

clean:
	@rm -rf "$(BIN_DIR)" "$(BUILD_DIR)"
	@echo "OK clean"
