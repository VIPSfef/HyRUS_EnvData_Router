px4_add_module(
    MODULE systemcmds__shtc3_probe
    MAIN shtc3_probe
    STACK_MAIN 2048
    SRCS
        shtc3_probe.cpp
        SHTC3Probe.hpp
    INCLUDES
        ${PX4_SOURCE_DIR}/src/drivers/hygrometer/shtc3
)
