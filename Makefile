# Optional backends are opt-in; ACL, yyjson and xxHash are required.
BUILD_WITH_C11 ?= NO
HAS_ROCKSDB ?= NO
HAS_WT ?= NO
HAS_TBB ?= NO
HAS_JEMALLOC ?= NO
PKV_COMPACT_ZSET ?= YES
PKV_COMPACT_SET_LEAF ?= YES
PKV_COMPACT_SET_FRAGMENTS ?= YES
BUILD_DIR ?= build
JOBS ?= 4
CMAKE_ARGS ?=
PKV_PGO_PROFILE ?=

# Per-machine dependency options; command-line variables take precedence.
-include Makefile.local

BUILD_ARGS = -DBUILD_WITH_C11=$(BUILD_WITH_C11) -DHAS_ROCKSDB=$(HAS_ROCKSDB) \
             -DHAS_WT=$(HAS_WT) -DHAS_TBB=$(HAS_TBB) -DHAS_JEMALLOC=$(HAS_JEMALLOC) \
             -DPKV_COMPACT_ZSET=$(PKV_COMPACT_ZSET) \
             -DPKV_COMPACT_SET_LEAF=$(PKV_COMPACT_SET_LEAF) \
             -DPKV_COMPACT_SET_FRAGMENTS=$(PKV_COMPACT_SET_FRAGMENTS) \
             -DPKV_PGO_PROFILE="$(PKV_PGO_PROFILE)"

.PHONY: all clean cl help h rebuild rb test
all:
	cmake -S . -B "$(BUILD_DIR)" $(BUILD_ARGS) -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS)
	cmake --build "$(BUILD_DIR)" -j $(JOBS)

test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

clean cl:
	rm -rf "$(BUILD_DIR)"

help h:
	@echo 'make [BUILD_WITH_C11=YES] [HAS_ROCKSDB=YES] [HAS_WT=YES] [HAS_TBB=YES]'
	@echo 'Binary: $(BUILD_DIR)/pkv; pass dependency paths with CMAKE_ARGS.'
	@echo 'Disable new compact ZSets: make PKV_COMPACT_ZSET=NO'
	@echo 'Disable new short Set leaves: make PKV_COMPACT_SET_LEAF=NO'
	@echo 'Disable compact Set fragments: make PKV_COMPACT_SET_FRAGMENTS=NO'
	@echo 'Clang PGO: make BUILD_DIR=build-pgo PKV_PGO_PROFILE=/path/to/pkv.profdata'

rebuild rb:
	$(MAKE) clean
	$(MAKE) all
