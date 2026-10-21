px4_add_module(
	MODULE drivers__hygrometer__shtc3
	MAIN shtc3
	SRCS
		SHTC3.cpp
		SHTC3.hpp
		SHTC3Protocol.hpp
	MODULE_CONFIG
		shtc3_params.yaml
	DEPENDS
		drivers__device
		px4_work_queue
)
