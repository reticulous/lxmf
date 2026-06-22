# lxmf — contribute a read-only /fixed file: the message-notification sound
# (data/lxmf/ding.wav -> /fixed/lxmf/ding.wav). spangap-core's
# spangap_create_factory_image() merges every dir appended to
# SPANGAP_EXTRA_DATA_DIRS after the core defaults and before the consumer's own
# data/, so the buildable can still override. CMAKE_CURRENT_LIST_DIR is this
# staged component dir; its data/ is a symlink to the straddle's esp-idf/data/.
set_property(GLOBAL APPEND PROPERTY SPANGAP_EXTRA_DATA_DIRS
            "${CMAKE_CURRENT_LIST_DIR}/data")
