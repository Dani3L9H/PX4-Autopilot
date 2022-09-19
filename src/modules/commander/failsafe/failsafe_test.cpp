/****************************************************************************
 *
 *   Copyright (c) 2022 PX4 Development Team. All rights reserved.
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

#include <gtest/gtest.h>

#include "framework.h"
#include <uORB/topics/vehicle_status.h>

// to run: make tests TESTFILTER=failsafe_test

using namespace time_literals;

class FailsafeTester : public FailsafeBase
{
public:
	FailsafeTester(ModuleParams *parent) : FailsafeBase(parent) {}

protected:

	void checkStateAndMode(const hrt_abstime &time_us, const State &state,
			       const vehicle_status_flags_s &status_flags) override
	{
		CHECK_FAILSAFE(status_flags, manual_control_signal_lost,
			       ActionOptions(Action::RTL).clearOn(ClearCondition::OnModeChangeOrDisarm));
		CHECK_FAILSAFE(status_flags, gcs_connection_lost, Action::Descend);

		if (state.user_intended_mode == vehicle_status_s::NAVIGATION_STATE_AUTO_MISSION) {
			CHECK_FAILSAFE(status_flags, mission_failure, Action::Descend);
		}

		CHECK_FAILSAFE(status_flags, wind_limit_exceeded,
			       ActionOptions(Action::RTL).allowUserTakeover(UserTakeoverAllowed::Never));

		_last_state_test = checkFailsafe(_caller_id_test, _last_state_test, status_flags.fd_motor_failure
						 && status_flags.fd_critical_failure, Action::Terminate);
	}

	Action checkModeFallback(const vehicle_status_flags_s &status_flags, uint8_t user_intended_mode) const override
	{
		return Action::None;
	}

private:
	const int _caller_id_test{genCallerId()};
	bool _last_state_test{false};
};

class FailsafeTest : public ::testing::Test

{
public:

	void SetUp() override
	{
		param_control_autosave(false);
		param_t param = param_handle(px4::params::COM_FAIL_ACT_T);

		float hold_delay = 5.f;
		param_set(param, &hold_delay);
	}

};


TEST_F(FailsafeTest, general)
{
	FailsafeTester failsafe(nullptr);

	vehicle_status_flags_s failsafe_flags{};
	FailsafeBase::State state{};
	state.armed = true;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_POSCTL;
	state.vehicle_type = vehicle_status_s::VEHICLE_TYPE_ROTARY_WING;
	hrt_abstime time = 5_s;
	bool stick_override_request = false;

	uint8_t updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);

	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::None);

	// RC lost -> Hold, then RTL
	time += 10_ms;
	failsafe_flags.manual_control_signal_lost = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Hold);
	time += 6_s;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::RTL);

	// DL link lost -> Descend
	time += 10_ms;
	failsafe_flags.gcs_connection_lost = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Descend);

	// DL link regained -> RTL (RC still lost)
	time += 10_ms;
	failsafe_flags.gcs_connection_lost = false;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::RTL);

	// RC lost cleared -> keep RTL
	time += 10_ms;
	failsafe_flags.manual_control_signal_lost = false;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::RTL);

	// Mode change -> clear failsafe
	time += 10_ms;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_ALTCTL;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::None);
}

TEST_F(FailsafeTest, takeover)
{
	FailsafeTester failsafe(nullptr);

	vehicle_status_flags_s failsafe_flags{};
	FailsafeBase::State state{};
	state.armed = true;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_POSCTL;
	state.vehicle_type = vehicle_status_s::VEHICLE_TYPE_ROTARY_WING;
	hrt_abstime time = 3847124342;
	bool stick_override_request = false;

	uint8_t updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);

	// Mission failure -> no failsafe
	time += 10_ms;
	failsafe_flags.mission_failure = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::None);

	// Change to mission -> Hold, then Descend
	time += 10_ms;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_AUTO_MISSION;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Hold);
	// Try stick movement during hold -> must be denied
	time += 3_s;
	stick_override_request = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	stick_override_request = false;
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Hold);
	// Now Descend
	time += 3_s;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Descend);

	// Move sticks -> user takeover active
	time += 10_ms;
	stick_override_request = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_POSCTL;
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Warn);
	ASSERT_TRUE(failsafe.userTakeoverActive());
	stick_override_request = false;

	// Now the failsafe must clear as we switched mode
	time += 10_ms;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::None);
	ASSERT_FALSE(failsafe.userTakeoverActive());
}

TEST_F(FailsafeTest, takeover_denied)
{
	FailsafeTester failsafe(nullptr);

	vehicle_status_flags_s failsafe_flags{};
	FailsafeBase::State state{};
	state.armed = true;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_POSCTL;
	state.vehicle_type = vehicle_status_s::VEHICLE_TYPE_ROTARY_WING;
	hrt_abstime time = 3847124342;
	bool stick_override_request = false;

	uint8_t updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);

	// Wind limit exceeded -> RTL w/o delay and denying takeover
	time += 10_ms;
	failsafe_flags.wind_limit_exceeded = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::RTL);
	// Try takeover (mode switch + stick movements)
	time += 10_ms;
	stick_override_request = true;
	state.user_intended_mode = vehicle_status_s::NAVIGATION_STATE_STAB;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	stick_override_request = false;
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::RTL);

	// Test termination
	failsafe_flags.fd_motor_failure = true;
	failsafe_flags.fd_critical_failure = true;
	time += 10_ms;
	stick_override_request = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	stick_override_request = false;
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Terminate);

	// Termination is final
	failsafe_flags.wind_limit_exceeded = false;
	failsafe_flags.fd_motor_failure = false;
	failsafe_flags.fd_critical_failure = false;
	state.armed = false;
	time += 10_ms;
	stick_override_request = true;
	updated_user_intented_mode = failsafe.update(time, state, false, stick_override_request, failsafe_flags);
	stick_override_request = false;
	ASSERT_EQ(updated_user_intented_mode, state.user_intended_mode);
	ASSERT_EQ(failsafe.selectedAction(), FailsafeBase::Action::Terminate);
}

