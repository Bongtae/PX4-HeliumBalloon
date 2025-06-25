/****************************************************************************
 *
 *   Copyright (c) 2013-2019 PX4 Development Team. All rights reserved.
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

/**
 * @file ControlAllocator.cpp
 *
 * Control allocator.
 *
 * @author Pedro Roque <padr@kth.se>
 */

#include "ControlAllocator.hpp"

#include <drivers/drv_hrt.h>
#include <circuit_breaker/circuit_breaker.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>

// PX4 LOG
// #include <px4_platform_common/log.h>

using namespace matrix;
using namespace time_literals;

SpacecraftControlAllocator::SpacecraftControlAllocator(ModuleParams *parent) :
	ModuleParams(nullptr)
{
	_control_allocator_status_pub[0].advertise();
	_control_allocator_status_pub[1].advertise();

	_actuator_motors_pub.advertise();
	_actuator_servos_pub.advertise();
	_actuator_servos_trim_pub.advertise();

	for (int i = 0; i < MAX_NUM_MOTORS; ++i) {
		char buffer[17];
		snprintf(buffer, sizeof(buffer), "CA_R%u_SLEW", i);
		_param_handles.slew_rate_motors[i] = param_find(buffer);
	}

	updateParams();
}

SpacecraftControlAllocator::~SpacecraftControlAllocator()
{
	for (int i = 0; i < SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES; ++i) {
		delete _control_allocation[i];
	}

	delete _actuator_effectiveness;

}

void
SpacecraftControlAllocator::updateParams()
{
	ModuleParams::updateParams();
	_has_slew_rate = false;

	for (int i = 0; i < MAX_NUM_MOTORS; ++i) {
		param_get(_param_handles.slew_rate_motors[i], &_params.slew_rate_motors[i]);
		_has_slew_rate |= _params.slew_rate_motors[i] > FLT_EPSILON;
	}

	// Allocation method & effectiveness source
	// Do this first: in case a new method is loaded, it will be configured below
	bool updated = update_effectiveness_source();
	update_allocation_method(updated); // must be called after update_effectiveness_source()

	if (_num_control_allocation == 0) {
		return;
	}

	for (int i = 0; i < _num_control_allocation; ++i) {
		_control_allocation[i]->updateParameters();
	}

	update_effectiveness_matrix_if_needed(SpacecraftEffectivenessUpdateReason::CONFIGURATION_UPDATE);
}

void
SpacecraftControlAllocator::update_allocation_method(bool force)
{
	SpacecraftAllocationMethod configured_method = (SpacecraftAllocationMethod)_param_ca_method.get();

	if (!_actuator_effectiveness) {
		PX4_ERR("_actuator_effectiveness null");
		return;
	}

	if (_allocation_method_id != configured_method || force) {

		matrix::Vector<float, NUM_ACTUATORS> actuator_sp[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];

		// Cleanup first
		for (int i = 0; i < SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES; ++i) {
			// Save current state
			if (_control_allocation[i] != nullptr) {
				actuator_sp[i] = _control_allocation[i]->getActuatorSetpoint();
			}

			delete _control_allocation[i];
			_control_allocation[i] = nullptr;
		}

		_num_control_allocation = _actuator_effectiveness->numMatrices();

		SpacecraftAllocationMethod desired_methods[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];
		_actuator_effectiveness->getDesiredSpacecraftAllocationMethod(desired_methods);

		bool normalize_rpy[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];
		_actuator_effectiveness->getNormalizeRPY(normalize_rpy);

		for (int i = 0; i < _num_control_allocation; ++i) {
			SpacecraftAllocationMethod method = configured_method;

			if (configured_method == SpacecraftAllocationMethod::AUTO) {
				method = desired_methods[i];
			}

			switch (method) {
			case SpacecraftAllocationMethod::PSEUDO_INVERSE:
				_control_allocation[i] = new ControlAllocationPseudoInverse();
				break;

			default:
				PX4_ERR("Allocation method not supported");
				break;
			}

			if (_control_allocation[i] == nullptr) {
				PX4_ERR("alloc failed");
				_num_control_allocation = 0;

			} else {
				_control_allocation[i]->setNormalizeRPY(normalize_rpy[i]);
				_control_allocation[i]->setActuatorSetpoint(actuator_sp[i]);
			}
		}

		_allocation_method_id = configured_method;
	}
}

bool
SpacecraftControlAllocator::update_effectiveness_source()
{
	const EffectivenessSource source = (EffectivenessSource)_param_ca_airframe.get();

	if (_effectiveness_source_id != source) {

		// try to instanciate new effectiveness source
		SpacecraftActuatorEffectiveness *tmp = nullptr;

		switch (source) {
		case EffectivenessSource::SPACECRAFT_2D:
		case EffectivenessSource::SPACECRAFT_3D:
			tmp = new ActuatorEffectivenessSpacecraft(this);
			break;

		default:
			PX4_ERR("Airframe is not Spacecraft. Please build the correct vehicle platform.");
			break;
		}

		// Replace previous source with new one
		if (tmp == nullptr) {
			// It did not work, forget about it
			PX4_ERR("Actuator effectiveness init failed");
			_param_ca_airframe.set((int)_effectiveness_source_id);

		} else {
			// Swap effectiveness sources
			delete _actuator_effectiveness;
			_actuator_effectiveness = tmp;

			// Save source id
			_effectiveness_source_id = source;
		}

		return true;
	}

	return false;
}

void
SpacecraftControlAllocator::generateActuationSignals()
{

#ifndef ENABLE_LOCKSTEP_SCHEDULER // Backup schedule would interfere with lockstep
	// Push backup schedule
	ScheduleDelayed(50_ms);
#endif

	if (_num_control_allocation == 0 || _actuator_effectiveness == nullptr) {
		return;
	}

	{
		vehicle_status_s vehicle_status;

		if (_vehicle_status_sub.update(&vehicle_status)) {

			_armed = vehicle_status.arming_state == vehicle_status_s::ARMING_STATE_ARMED;
		}
	}

	{
		vehicle_control_mode_s vehicle_control_mode;

		if (_vehicle_control_mode_sub.update(&vehicle_control_mode)) {
			_publish_controls = vehicle_control_mode.flag_control_allocation_enabled;
		}
	}

	// Guard against too small (< 0.2ms) and too large (> 20ms) dt's.
	const hrt_abstime now = hrt_absolute_time();
	const float dt = math::constrain(((now - _last_run) / 1e6f), 0.0002f, 0.02f);

	bool do_update = false;
	vehicle_torque_setpoint_s vehicle_torque_setpoint;
	vehicle_thrust_setpoint_s vehicle_thrust_setpoint;

	// Run allocator on torque changes
	if (_vehicle_torque_setpoint_sub.update(&vehicle_torque_setpoint)) {
		_torque_sp = matrix::Vector3f(vehicle_torque_setpoint.xyz);

		do_update = true;
		_timestamp_sample = vehicle_torque_setpoint.timestamp_sample;

	}

	// Also run allocator on thrust setpoint changes if the torque setpoint
	// has not been updated for more than 5ms
	if (_vehicle_thrust_setpoint_sub.update(&vehicle_thrust_setpoint)) {
		_thrust_sp = matrix::Vector3f(vehicle_thrust_setpoint.xyz);

		if (dt > 0.005f) {
			do_update = true;
			_timestamp_sample = vehicle_thrust_setpoint.timestamp_sample;
		}
	}

	if (do_update) {
		_last_run = now;

		check_for_motor_failures();

		update_effectiveness_matrix_if_needed(SpacecraftEffectivenessUpdateReason::NO_EXTERNAL_UPDATE);

		// Set control setpoint vector(s)
		matrix::Vector<float, NUM_AXES> c[ActuatorEffectiveness::MAX_NUM_MATRICES];
		c[0](0) = _torque_sp(0);
		c[0](1) = _torque_sp(1);
		c[0](2) = _torque_sp(2);
		c[0](3) = _thrust_sp(0);
		c[0](4) = _thrust_sp(1);
		c[0](5) = _thrust_sp(2);

		if (_num_control_allocation > 1) {
			if (_vehicle_torque_setpoint1_sub.copy(&vehicle_torque_setpoint)) {
				c[1](0) = vehicle_torque_setpoint.xyz[0];
				c[1](1) = vehicle_torque_setpoint.xyz[1];
				c[1](2) = vehicle_torque_setpoint.xyz[2];
			}

			if (_vehicle_thrust_setpoint1_sub.copy(&vehicle_thrust_setpoint)) {
				c[1](3) = vehicle_thrust_setpoint.xyz[0];
				c[1](4) = vehicle_thrust_setpoint.xyz[1];
				c[1](5) = vehicle_thrust_setpoint.xyz[2];
			}
		}

		for (int i = 0; i < _num_control_allocation; ++i) {

			_control_allocation[i]->setControlSetpoint(c[i]);

			// Do allocation
			_control_allocation[i]->allocate();
			_actuator_effectiveness->allocateAuxilaryControls(dt, i, _control_allocation[i]->_actuator_sp); //flaps and spoilers
			_actuator_effectiveness->updateSetpoint(c[i], i, _control_allocation[i]->_actuator_sp,
								_control_allocation[i]->getActuatorMin(), _control_allocation[i]->getActuatorMax());

			if (_has_slew_rate) {
				_control_allocation[i]->applySlewRateLimit(dt);
			}

			// PX4_INFO("Actuator setpoint %d: %f %f %f %f %f %f %f %f", i,
			// 	 (double)_control_allocation[i]->_actuator_sp(0), (double)_control_allocation[i]->_actuator_sp(1),
			// 	 (double)_control_allocation[i]->_actuator_sp(2), (double)_control_allocation[i]->_actuator_sp(3),
			// 	 (double)_control_allocation[i]->_actuator_sp(4), (double)_control_allocation[i]->_actuator_sp(5),
			// 	 (double)_control_allocation[i]->_actuator_sp(6), (double)_control_allocation[i]->_actuator_sp(7));
			_control_allocation[i]->clipActuatorSetpoint();
		}
	}

	// Publish actuator setpoint and allocator status
	publish_actuator_controls();

	// Publish status at limited rate, as it's somewhat expensive and we use it for slower dynamics
	// (i.e. anti-integrator windup)
	if (now - _last_status_pub >= 5_ms) {
		publish_control_allocator_status(0);

		if (_num_control_allocation > 1) {
			publish_control_allocator_status(1);
		}

		_last_status_pub = now;
	}
}

void
SpacecraftControlAllocator::update_effectiveness_matrix_if_needed(SpacecraftEffectivenessUpdateReason reason)
{
	SpacecraftActuatorEffectiveness::Configuration config{};

	if (reason == SpacecraftEffectivenessUpdateReason::NO_EXTERNAL_UPDATE
	    && hrt_elapsed_time(&_last_effectiveness_update) < 100_ms) { // rate-limit updates
		return;
	}

	if (_actuator_effectiveness->getEffectivenessMatrix(config, reason)) {
		_last_effectiveness_update = hrt_absolute_time();

		memcpy(_control_allocation_selection_indexes, config.matrix_selection_indexes,
		       sizeof(_control_allocation_selection_indexes));

		// Get the minimum and maximum depending on type and configuration
		SpacecraftActuatorEffectiveness::ActuatorVector minimum[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];
		SpacecraftActuatorEffectiveness::ActuatorVector maximum[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];
		SpacecraftActuatorEffectiveness::ActuatorVector slew_rate[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES];
		int actuator_idx = 0;
		int actuator_idx_matrix[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES] {};

		actuator_servos_trim_s trims{};
		static_assert(actuator_servos_trim_s::NUM_CONTROLS == actuator_servos_s::NUM_CONTROLS, "size mismatch");

		for (int actuator_type = 0; actuator_type < (int)SpacecraftActuatorType::COUNT; ++actuator_type) {
			_num_actuators[actuator_type] = config.num_actuators[actuator_type];

			for (int actuator_type_idx = 0; actuator_type_idx < config.num_actuators[actuator_type]; ++actuator_type_idx) {
				if (actuator_idx >= NUM_ACTUATORS) {
					_num_actuators[actuator_type] = 0;
					PX4_ERR("Too many actuators");
					break;
				}

				int selected_matrix = _control_allocation_selection_indexes[actuator_idx];

				if ((SpacecraftActuatorType)actuator_type == SpacecraftActuatorType::MOTORS) {
					if (actuator_type_idx >= MAX_NUM_MOTORS) {
						PX4_ERR("Too many motors");
						_num_actuators[actuator_type] = 0;
						break;
					}

					if (_param_r_rev.get() & (1u << actuator_type_idx)) {
						minimum[selected_matrix](actuator_idx_matrix[selected_matrix]) = -1.f;

					} else {
						minimum[selected_matrix](actuator_idx_matrix[selected_matrix]) = 0.f;
					}

					slew_rate[selected_matrix](actuator_idx_matrix[selected_matrix]) = _params.slew_rate_motors[actuator_type_idx];

				} else if ((SpacecraftActuatorType)actuator_type == SpacecraftActuatorType::THRUSTERS) {
					if (actuator_type_idx >= MAX_NUM_THRUSTERS) {
						PX4_ERR("Too many thrusters");
						_num_actuators[actuator_type] = 0;
						break;
					}

					minimum[selected_matrix](actuator_idx_matrix[selected_matrix]) = 0.f;

				} else if ((SpacecraftActuatorType)actuator_type == SpacecraftActuatorType::SERVOS) {
					if (actuator_type_idx >= MAX_NUM_SERVOS) {
						PX4_ERR("Too many servos");
						_num_actuators[actuator_type] = 0;
						break;
					}

					minimum[selected_matrix](actuator_idx_matrix[selected_matrix]) = -1.f;
					slew_rate[selected_matrix](actuator_idx_matrix[selected_matrix]) = _params.slew_rate_servos[actuator_type_idx];
					trims.trim[actuator_type_idx] = config.trim[selected_matrix](actuator_idx_matrix[selected_matrix]);

				} else {
					minimum[selected_matrix](actuator_idx_matrix[selected_matrix]) = -1.f;
				}

				maximum[selected_matrix](actuator_idx_matrix[selected_matrix]) = 1.f;

				++actuator_idx_matrix[selected_matrix];
				++actuator_idx;
			}
		}

		// Handle failed actuators
		if (_handled_motor_failure_bitmask) {
			actuator_idx = 0;
			memset(&actuator_idx_matrix, 0, sizeof(actuator_idx_matrix));

			for (int motors_idx = 0; motors_idx < _num_actuators[0] && motors_idx < actuator_motors_s::NUM_CONTROLS; motors_idx++) {
				int selected_matrix = _control_allocation_selection_indexes[actuator_idx];

				if (_handled_motor_failure_bitmask & (1 << motors_idx)) {
					SpacecraftActuatorEffectiveness::EffectivenessMatrix &matrix = config.effectiveness_matrices[selected_matrix];

					for (int i = 0; i < NUM_AXES; i++) {
						matrix(i, actuator_idx_matrix[selected_matrix]) = 0.0f;
					}
				}

				++actuator_idx_matrix[selected_matrix];
				++actuator_idx;
			}
		}

		for (int i = 0; i < _num_control_allocation; ++i) {
			_control_allocation[i]->setActuatorMin(minimum[i]);
			_control_allocation[i]->setActuatorMax(maximum[i]);
			_control_allocation[i]->setSlewRateLimit(slew_rate[i]);

			// Set all the elements of a row to 0 if that row has weak authority.
			// That ensures that the algorithm doesn't try to control axes with only marginal control authority,
			// which in turn would degrade the control of the main axes that actually should and can be controlled.

			SpacecraftActuatorEffectiveness::EffectivenessMatrix &matrix = config.effectiveness_matrices[i];

			for (int n = 0; n < NUM_AXES; n++) {
				bool all_entries_small = true;

				for (int m = 0; m < config.num_actuators_matrix[i]; m++) {
					if (fabsf(matrix(n, m)) > 0.05f) {
						all_entries_small = false;
					}
				}

				if (all_entries_small) {
					matrix.row(n) = 0.f;
				}
			}

			// Assign control effectiveness matrix
			int total_num_actuators = config.num_actuators_matrix[i];
			_control_allocation[i]->setEffectivenessMatrix(config.effectiveness_matrices[i], config.trim[i],
					config.linearization_point[i], total_num_actuators,
					reason == SpacecraftEffectivenessUpdateReason::CONFIGURATION_UPDATE);
		}

		trims.timestamp = hrt_absolute_time();
		_actuator_servos_trim_pub.publish(trims);
	}
}

void
SpacecraftControlAllocator::publish_control_allocator_status(int matrix_index)
{
	control_allocator_status_s control_allocator_status{};
	control_allocator_status.timestamp = hrt_absolute_time();

	// TODO: disabled motors (?)

	// Allocated control
	const matrix::Vector<float, NUM_AXES> &allocated_control = _control_allocation[matrix_index]->getAllocatedControl();

	// Unallocated control
	const matrix::Vector<float, NUM_AXES> unallocated_control = _control_allocation[matrix_index]->getControlSetpoint() -
			allocated_control;
	control_allocator_status.unallocated_torque[0] = unallocated_control(0);
	control_allocator_status.unallocated_torque[1] = unallocated_control(1);
	control_allocator_status.unallocated_torque[2] = unallocated_control(2);
	control_allocator_status.unallocated_thrust[0] = unallocated_control(3);
	control_allocator_status.unallocated_thrust[1] = unallocated_control(4);
	control_allocator_status.unallocated_thrust[2] = unallocated_control(5);

	// override control_allocator_status in customized saturation logic for certain effectiveness types
	_actuator_effectiveness->getUnallocatedControl(matrix_index, control_allocator_status);

	// Allocation success flags
	control_allocator_status.torque_setpoint_achieved = (Vector3f(control_allocator_status.unallocated_torque[0],
			control_allocator_status.unallocated_torque[1],
			control_allocator_status.unallocated_torque[2]).norm_squared() < 1e-6f);
	control_allocator_status.thrust_setpoint_achieved = (Vector3f(control_allocator_status.unallocated_thrust[0],
			control_allocator_status.unallocated_thrust[1],
			control_allocator_status.unallocated_thrust[2]).norm_squared() < 1e-6f);

	// Actuator saturation
	const matrix::Vector<float, NUM_ACTUATORS> &actuator_sp = _control_allocation[matrix_index]->getActuatorSetpoint();
	const matrix::Vector<float, NUM_ACTUATORS> &actuator_min = _control_allocation[matrix_index]->getActuatorMin();
	const matrix::Vector<float, NUM_ACTUATORS> &actuator_max = _control_allocation[matrix_index]->getActuatorMax();

	for (int i = 0; i < NUM_ACTUATORS; i++) {
		if (actuator_sp(i) > (actuator_max(i) - FLT_EPSILON)) {
			control_allocator_status.actuator_saturation[i] = control_allocator_status_s::ACTUATOR_SATURATION_UPPER;

		} else if (actuator_sp(i) < (actuator_min(i) + FLT_EPSILON)) {
			control_allocator_status.actuator_saturation[i] = control_allocator_status_s::ACTUATOR_SATURATION_LOWER;
		}
	}

	// Handled motor failures
	control_allocator_status.handled_motor_failure_mask = _handled_motor_failure_bitmask;

	_control_allocator_status_pub[matrix_index].publish(control_allocator_status);
}

void
SpacecraftControlAllocator::publish_actuator_controls()
{
	if (!_publish_controls) {
		return;
	}

	actuator_motors_s actuator_motors;
	actuator_motors.timestamp = hrt_absolute_time();
	actuator_motors.timestamp_sample = _timestamp_sample;

	actuator_servos_s actuator_servos;
	actuator_servos.timestamp = actuator_motors.timestamp;
	actuator_servos.timestamp_sample = _timestamp_sample;

	actuator_motors.reversible_flags = _param_r_rev.get();

	int actuator_idx = 0;
	int actuator_idx_matrix[SpacecraftActuatorEffectiveness::MAX_NUM_MATRICES] {};

	uint32_t stopped_motors = _actuator_effectiveness->getStoppedMotors() | _handled_motor_failure_bitmask;

	// Setpoint for motors and servos
	int actuator_type = 0;

	if (_num_actuators[(int)SpacecraftActuatorType::THRUSTERS] > 0) {
		actuator_type = (int)SpacecraftActuatorType::THRUSTERS;
	}

	// motors
	int motors_idx;

	for (motors_idx = 0; motors_idx < _num_actuators[actuator_type]
	     && motors_idx < actuator_motors_s::NUM_CONTROLS; motors_idx++) {
		int selected_matrix = _control_allocation_selection_indexes[actuator_idx];
		float actuator_sp = _control_allocation[selected_matrix]->getActuatorSetpoint()(actuator_idx_matrix[selected_matrix]);
		actuator_motors.control[motors_idx] = PX4_ISFINITE(actuator_sp) ? actuator_sp : NAN;

		if (stopped_motors & (1u << motors_idx)) {
			actuator_motors.control[motors_idx] = NAN;
		}

		++actuator_idx_matrix[selected_matrix];
		++actuator_idx;
	}

	for (int i = motors_idx; i < actuator_motors_s::NUM_CONTROLS; i++) {
		actuator_motors.control[i] = NAN;
	}

	_actuator_motors_pub.publish(actuator_motors);

	// servos
	if (_num_actuators[1] > 0) {
		int servos_idx;

		for (servos_idx = 0; servos_idx < _num_actuators[1] && servos_idx < actuator_servos_s::NUM_CONTROLS; servos_idx++) {
			int selected_matrix = _control_allocation_selection_indexes[actuator_idx];
			float actuator_sp = _control_allocation[selected_matrix]->getActuatorSetpoint()(actuator_idx_matrix[selected_matrix]);
			actuator_servos.control[servos_idx] = PX4_ISFINITE(actuator_sp) ? actuator_sp : NAN;
			++actuator_idx_matrix[selected_matrix];
			++actuator_idx;
		}

		for (int i = servos_idx; i < actuator_servos_s::NUM_CONTROLS; i++) {
			actuator_servos.control[i] = NAN;
		}

		_actuator_servos_pub.publish(actuator_servos);
	}
}

void
SpacecraftControlAllocator::check_for_motor_failures()
{
	failure_detector_status_s failure_detector_status;

	if ((FailureMode)_param_ca_failure_mode.get() > FailureMode::IGNORE
	    && _failure_detector_status_sub.update(&failure_detector_status)) {
		if (failure_detector_status.fd_motor) {

			if (_handled_motor_failure_bitmask != failure_detector_status.motor_failure_mask) {
				// motor failure bitmask changed
				switch ((FailureMode)_param_ca_failure_mode.get()) {
				case FailureMode::REMOVE_FIRST_FAILING_MOTOR: {
						// Count number of failed motors
						const int num_motors_failed = math::countSetBits(failure_detector_status.motor_failure_mask);

						// Only handle if it is the first failure
						if (_handled_motor_failure_bitmask == 0 && num_motors_failed == 1) {
							_handled_motor_failure_bitmask = failure_detector_status.motor_failure_mask;
							PX4_WARN("Removing motor from allocation (0x%x)", _handled_motor_failure_bitmask);

							for (int i = 0; i < _num_control_allocation; ++i) {
								_control_allocation[i]->setHadActuatorFailure(true);
							}

							update_effectiveness_matrix_if_needed(SpacecraftEffectivenessUpdateReason::MOTOR_ACTIVATION_UPDATE);
						}
					}
					break;

				default:
					break;
				}

			}

		} else if (_handled_motor_failure_bitmask != 0) {
			// Clear bitmask completely
			PX4_INFO("Restoring all motors");
			_handled_motor_failure_bitmask = 0;

			for (int i = 0; i < _num_control_allocation; ++i) {
				_control_allocation[i]->setHadActuatorFailure(false);
			}

			update_effectiveness_matrix_if_needed(SpacecraftEffectivenessUpdateReason::MOTOR_ACTIVATION_UPDATE);
		}
	}
}

int SpacecraftControlAllocator::print_status()
{
	PX4_INFO("Running");

	// Print current allocation method
	switch (_allocation_method_id) {
	case SpacecraftAllocationMethod::NONE:
		PX4_INFO("Method: None");
		break;

	case SpacecraftAllocationMethod::PSEUDO_INVERSE:
		PX4_INFO("Method: Pseudo-inverse");
		break;

	case SpacecraftAllocationMethod::SEQUENTIAL_DESATURATION:
		PX4_INFO("Method: Sequential desaturation");
		break;

	case SpacecraftAllocationMethod::AUTO:
		PX4_INFO("Method: Auto");
		break;
	}

	// Print current airframe
	if (_actuator_effectiveness != nullptr) {
		PX4_INFO("Effectiveness Source: %s", _actuator_effectiveness->name());
	}

	// Print current effectiveness matrix
	for (int i = 0; i < _num_control_allocation; ++i) {
		const SpacecraftActuatorEffectiveness::EffectivenessMatrix &effectiveness =
			_control_allocation[i]->getEffectivenessMatrix();

		if (_num_control_allocation > 1) {
			PX4_INFO("Instance: %i", i);
		}

		PX4_INFO("  Effectiveness.T =");
		effectiveness.T().print();
		PX4_INFO("  minimum =");
		_control_allocation[i]->getActuatorMin().T().print();
		PX4_INFO("  maximum =");
		_control_allocation[i]->getActuatorMax().T().print();
		PX4_INFO("  Configured actuators: %i", _control_allocation[i]->numConfiguredActuators());
	}

	if (_handled_motor_failure_bitmask) {
		PX4_INFO("Failed motors: %i (0x%x)", math::countSetBits(_handled_motor_failure_bitmask),
			 _handled_motor_failure_bitmask);
	}

	return 0;
}
