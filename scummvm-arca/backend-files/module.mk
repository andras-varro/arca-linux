MODULE := backends/platform/arca

MODULE_OBJS := \
	arca.o \
	arca-graphics.o \
	arca-events.o \
	../../mixer/oss/oss-mixer.o

# We don't use rules.mk but rather manually update OBJS and MODULE_DIRS.
MODULE_OBJS := $(addprefix $(MODULE)/, $(MODULE_OBJS))
OBJS := $(MODULE_OBJS) $(OBJS)
MODULE_DIRS += $(sort $(dir $(MODULE_OBJS)))

# (O10) Enable NEON SIMD for the graphics blit hot path
$(MODULE)/arca-graphics.o: CXXFLAGS += -mfpu=neon
