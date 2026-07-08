CXX      ?= g++
TARGET    = snowflake
SRCDIR    = src
BUILDDIR  = build
# qrcodegen is vendored (MIT) – zero external dependencies
SRCS      = $(SRCDIR)/client.cpp $(SRCDIR)/qrcodegen.cpp
OBJS      = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))

# -pthread is required for std::thread on Linux
BASE     = -std=c++17 -pthread

# size-optimized (default)
size: CXXFLAGS = $(BASE) -Os -s -Wall -Wextra -Wpedantic
size: $(BUILDDIR)/$(TARGET)

# performance build
perf: CXXFLAGS = $(BASE) -O3 -s -flto -Wall -Wextra -Wpedantic
perf: $(BUILDDIR)/$(TARGET)

# static build – fully self-contained
static: CXXFLAGS = $(BASE) -Os -s -static -Wall -Wextra -Wpedantic
static: $(BUILDDIR)/$(TARGET)

all: size

# macOS / Windows: build with the default target. On macOS no -pthread is
# needed (pthreads are in libc); on Windows use MinGW-w64
#   (x86_64-w64-mingw32-g++ -std=c++17 -O2 -o snowflake src/client.cpp src/qrcodegen.cpp -lws2_32)
$(BUILDDIR)/$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

debug: CXXFLAGS = $(BASE) -g -O0 -Wall -Wextra -Wpedantic -DDEBUG
debug: $(BUILDDIR)/$(TARGET)-debug

$(BUILDDIR)/$(TARGET)-debug: $(SRCS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILDDIR)

install: size
	$(BUILDDIR)/$(TARGET) install

.PHONY: all size perf static debug clean install
