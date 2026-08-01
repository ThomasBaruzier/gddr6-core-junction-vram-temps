CC ?= cc
PROGRAM ?= gputemps
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CPPFLAGS ?=
CFLAGS ?= -O3
LDFLAGS ?=
LDLIBS ?=

NVML_HEADER ?= $(firstword $(wildcard \
	$(CUDA_HOME)/include/nvml.h \
	$(CUDA_HOME)/targets/*/include/nvml.h \
	/usr/include/nvml.h \
	/usr/include/nvidia-ml/nvml.h \
	/usr/include/nvidia/gdk/nvml.h \
	/usr/local/cuda*/include/nvml.h \
	/usr/local/cuda*/targets/*/include/nvml.h \
	/opt/cuda*/include/nvml.h \
	/opt/cuda*/targets/*/include/nvml.h))

NVML_CPPFLAGS := $(if $(NVML_HEADER),-I$(dir $(NVML_HEADER)))
BASE_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic

LDLIBS += -lnvidia-ml -lpci

SOURCES := src/main.c src/monitor.c src/sensor.c src/mmio.c
OBJECTS := $(SOURCES:.c=.o)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all clean install uninstall check-nvml

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(NVML_CPPFLAGS) $(CFLAGS) $(BASE_CFLAGS) \
		-MMD -MP -c -o $@ $<

src/monitor.o src/sensor.o: | check-nvml

check-nvml:
	@printf '%s\n' '#include <nvml.h>' | \
		$(CC) $(CPPFLAGS) $(NVML_CPPFLAGS) -x c -E - >/dev/null 2>&1 || { \
		printf '%s%s%s\n' \
			'error: nvml.h not found; install the NVML development files, ' \
			'set CUDA_HOME or NVML_HEADER, or pass ' \
			'CPPFLAGS="-I/path/to/include"'; \
		exit 1; \
	}

install: $(PROGRAM)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROGRAM) $(DESTDIR)$(BINDIR)/$(PROGRAM)

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(PROGRAM)

clean:
	$(RM) $(PROGRAM) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
