# greenroom — C++17, no third-party deps. Win (MinGW-w64) + POSIX.
# make / make all   build dist/greenroom(.exe)
# make test         build and run the unit tests
# make clean        remove build artifacts
#
# Directory commands are shell-guarded because mingw32-make without a POSIX
# shell cannot exec `mkdir`/`rm` directly; `cmd /c` is a real executable, so
# it works through CreateProcess as well as through any sh. Backslash path
# rewriting is Windows-only: on POSIX a literal `..\dist` is one oddly
# named directory and the link step dies on a missing output path.

CXX ?= g++
CXXSTD ?= -std=c++17
WARN ?= -Wall -Wextra -O2
CXXFLAGS := $(CXXSTD) $(WARN)
BUILD := build
DIST := dist

SRCS := src/util.cpp src/sha256.cpp src/jsjson.cpp src/store.cpp src/http.cpp \
        src/api.cpp src/mcp.cpp src/cli.cpp src/main.cpp
LIB_SRCS := src/util.cpp src/sha256.cpp src/jsjson.cpp src/store.cpp src/http.cpp \
            src/api.cpp src/mcp.cpp src/cli.cpp
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.cpp=.o)))
LIB_OBJS := $(addprefix $(BUILD)/,$(notdir $(LIB_SRCS:.cpp=.o)))

BIN := $(DIST)/greenroom$(if $(filter $(OS),Windows_NT),.exe,)
TEST_BIN := $(BUILD)/test_unit$(if $(filter $(OS),Windows_NT),.exe,)

ifeq ($(OS),Windows_NT)
MD := cmd /d /c md
RMRF := cmd /d /c rmdir /s /q
FIXPATH = $(subst /,\,$1)
LDLIBS := -lws2_32
else
MD := mkdir -p
RMRF := rm -rf
FIXPATH = $1
LDLIBS := -lpthread
endif

vpath %.cpp src

.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJS) | $(DIST)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(DIST) $(BUILD):
	-$(MD) $(call FIXPATH,$@)

$(BUILD)/%.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc -c -o $@ $<

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_unit.cpp $(BUILD)/util.o $(BUILD)/sha256.o $(BUILD)/jsjson.o $(BUILD)/store.o $(BUILD)/http.o | $(BUILD)
	$(CXX) $(CXXFLAGS) -Isrc tests/test_unit.cpp $(BUILD)/util.o $(BUILD)/sha256.o $(BUILD)/jsjson.o $(BUILD)/store.o $(BUILD)/http.o -o $@ $(LDLIBS)

clean:
	-$(RMRF) $(call FIXPATH,$(BUILD))
	-$(RMRF) $(call FIXPATH,$(DIST))
