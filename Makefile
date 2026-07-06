CXX      ?= g++
TARGET    = snowflake
SRCDIR    = src
BUILDDIR  = build

# size-optimized (default) – 43 KB
size: CXXFLAGS = -std=c++17 -Os -s -Wall -Wextra -Wpedantic
size: $(BUILDDIR)/$(TARGET)

# performance build – 63 KB
perf: CXXFLAGS = -std=c++17 -O3 -s -flto -Wall -Wextra -Wpedantic
perf: $(BUILDDIR)/$(TARGET)

# static build – fully self-contained
static: CXXFLAGS = -std=c++17 -Os -s -static -Wall -Wextra -Wpedantic
static: $(BUILDDIR)/$(TARGET)

all: size

$(BUILDDIR)/$(TARGET): $(SRCDIR)/client.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

debug: CXXFLAGS = -std=c++17 -g -O0 -Wall -Wextra -Wpedantic -DDEBUG
debug: $(BUILDDIR)/$(TARGET)-debug
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/client.cpp

clean:
	rm -rf $(BUILDDIR)

install: size
	$(BUILDDIR)/$(TARGET) install

.PHONY: all size perf static debug clean install
