include ../Makefile.net_common

USEMODULE += od
USEMODULE += luid
USEMODULE += l2util
USEMODULE += ieee802154
USEMODULE += shell
USEMODULE += shell_cmds_default
USEMODULE += ps
USEMODULE += event_thread
USEMODULE += event_callback
USEMODULE += random
USEMODULE += ps
# TODO Remove this dependancy
# Only necessary for pulling the radio driver, no netdev code is actually added
USEMODULE += netdev_default
DISABLE_MODULE += netdev_ieee802154_submac netdev_ieee802154

USEMODULE += glossy

CFLAGS += -DEVENT_THREAD_MEDIUM_STACKSIZE=1024
CFLAGS += -DISR_STACKSIZE=2048
CFLAGS += -DEVENT_THREAD_HIGHEST_STACKSIZE=2048

include $(RIOTBASE)/Makefile.include

ifneq (,$(filter bhp,$(USEMODULE)))
  USEMODULE += bhp_event
endif

include $(RIOTMAKE)/default-radio-settings.inc.mk
