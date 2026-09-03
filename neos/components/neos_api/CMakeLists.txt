# Headers only. The implementations live in the firmware and reach apps
# through the syscall table; this component exists so that both sides compile
# against one definition of the ABI instead of two that can drift apart.
idf_component_register(INCLUDE_DIRS "include" REQUIRES ngl)
