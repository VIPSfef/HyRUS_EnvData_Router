/****************************************************************************
 *
 *   Copyright (c) 2021 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#ifndef SENS_ATMOS_HPP
#define SENS_ATMOS_HPP

#include <uORB/Subscription.hpp>
#include <uORB/topics/sensor_hygrometer.h>
#include <drivers/drv_hrt.h>

/** One atmospheric sensor pair, using the unmodified official ASLUAV message. */
class MavlinkStreamSensAtmos : public MavlinkStream
{
public:
	static MavlinkStream *new_instance(Mavlink *mavlink) { return new MavlinkStreamSensAtmos(mavlink); }

	static constexpr const char *get_name_static() { return "SENS_ATMOS"; }
	static constexpr uint16_t get_id_static() { return MAVLINK_MSG_ID_SENS_ATMOS; }

	const char *get_name() const override { return get_name_static(); }
	uint16_t get_id() override { return get_id_static(); }

	unsigned get_size() override
	{
		return mavlink_get_proto_version(_mavlink->get_channel()) == 2 && _sensor_hygrometer_sub.advertised()
		       ? MAVLINK_MSG_ID_SENS_ATMOS_LEN + MAVLINK_NUM_NON_PAYLOAD_BYTES : 0;
	}

private:
	explicit MavlinkStreamSensAtmos(Mavlink *mavlink) : MavlinkStream(mavlink) {}

	// SENS_ATMOS has no sensor ID: only instance zero may be sent on this component.
	uORB::Subscription _sensor_hygrometer_sub{ORB_ID(sensor_hygrometer), 0};
	static constexpr uint64_t MAX_SAMPLE_AGE_US = 1000000;

	bool send() override
	{
		// ID 8009 cannot be represented on MAVLink 1; do not consume or send there.
		if (mavlink_get_proto_version(_mavlink->get_channel()) != 2) { return false; }

		sensor_hygrometer_s sample{};

		if (!_sensor_hygrometer_sub.update(&sample)) {
			return false;
		}

		const uint64_t now = hrt_absolute_time();

		if (sample.timestamp_sample == 0 || sample.timestamp_sample > now
		    || now - sample.timestamp_sample > MAX_SAMPLE_AGE_US
		    || !PX4_ISFINITE(sample.temperature) || sample.temperature < -273.15f
		    || !PX4_ISFINITE(sample.humidity) || sample.humidity < 0.f || sample.humidity > 100.f) {
			return false;
		}

		mavlink_sens_atmos_t msg{};
		msg.timestamp = sample.timestamp_sample;
		msg.TempAmbient = sample.temperature; // degC, no scale factor
		msg.Humidity = sample.humidity; // percent RH, no scale factor
		mavlink_msg_sens_atmos_send_struct(_mavlink->get_channel(), &msg);
		return true;
	}
};

#endif // SENS_ATMOS_HPP
