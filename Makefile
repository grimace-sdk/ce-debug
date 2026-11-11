GRIMACE ?= $(error Set GRIMACE to a grimace checkout)
TOOLCHAIN = $(abspath $(GRIMACE))/cmake/toolchain.cmake
LLVM = $(if $(GRIMACE_LLVM_ROOT),-DGRIMACE_LLVM_ROOT=$(abspath $(GRIMACE_LLVM_ROOT)))
TARGETS = arm-ce3 arm-ce6-brain mips-ce1 mips-ce2 sh3-ce2

.PHONY: all check clean $(TARGETS)

all: $(TARGETS)

define target
$(1):
	cmake -S . -B build/$(1) -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(2) $(LLVM)
	cmake --build build/$(1)
endef

$(eval $(call target,arm-ce3,-DGRIMACE_ARCH=arm -DGRIMACE_CE_VERSION=3))
$(eval $(call target,arm-ce6-brain,-DGRIMACE_ARCH=arm -DGRIMACE_CE_VERSION=6 -DGRIMACE_PLATFORM=brain))
$(eval $(call target,mips-ce1,-DGRIMACE_ARCH=mips -DGRIMACE_CE_VERSION=1))
$(eval $(call target,mips-ce2,-DGRIMACE_ARCH=mips -DGRIMACE_CE_VERSION=2))
$(eval $(call target,sh3-ce2,-DGRIMACE_ARCH=sh3 -DGRIMACE_CE_VERSION=2))

check:
	python3 tests/check-agent.py

clean:
	rm -rf build
