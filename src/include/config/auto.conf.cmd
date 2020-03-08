deps_config := \
	lib/Kconfig \
	lib/Kconfig.debug \
	arch/arm64/Kconfig.debug \
	fs/Kconfig \
	drivers/irqchip/Kconfig \
	drivers/clocksource/Kconfig \
	drivers/block/Kconfig \
	drivers/of/Kconfig \
	drivers/Kconfig \
	net/Kconfig \
	fs/Kconfig.binfmt \
	mm/Kconfig \
	kernel/Kconfig.preempt \
	kernel/Kconfig.freezer \
	arch/Kconfig \
	init/Kconfig \
	arch/arm64/Kconfig \
	Kconfig

include/config/auto.conf: \
	$(deps_config)

ifneq "$(KERNELVERSION)" "0.0.1.1"
include/config/auto.conf: FORCE
endif
ifneq "$(ARCH)" "arm64"
include/config/auto.conf: FORCE
endif
ifneq "$(SRCARCH)" "arm64"
include/config/auto.conf: FORCE
endif

$(deps_config): ;
