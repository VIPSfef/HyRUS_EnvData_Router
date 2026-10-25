// Test-only uORB boundary; never opens a real subscription or a device.
#pragma once
#include <uORB/topics/sensor_hygrometer.h>
#define ORB_ID(sensor_hygrometer) 42
namespace fake
{
struct Topic {
	sensor_hygrometer_s sample{};
	bool advertised{false};
	bool unread{false};
};
extern Topic topics[2];
extern unsigned selected_instance;
extern unsigned selected_topic;
extern unsigned update_calls;
extern unsigned advertised_calls;
}
namespace uORB
{
class Subscription
{
public:
	Subscription(unsigned topic, unsigned instance) : _instance(instance)
	{
		fake::selected_topic = topic;
		fake::selected_instance = instance;
	}
	bool advertised() const
	{
		++fake::advertised_calls;
		return _instance < 2 && fake::topics[_instance].advertised;
	}
	bool update(sensor_hygrometer_s *sample)
	{
		++fake::update_calls;

		if (_instance >= 2 || !fake::topics[_instance].advertised || !fake::topics[_instance].unread) { return false; }

		*sample = fake::topics[_instance].sample;
		fake::topics[_instance].unread = false;
		return true;
	}
private:
	unsigned _instance;
};
}
