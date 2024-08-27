#include <limits>
#include "wheel_controller.h"
#include "robot_model.h"
#include "utils.h"
#include <algorithm>

#define kControlLoopPeriodSeconds 1e-2

#define kSpeedModelTimeConstant 0.29
#define kSpeedModelDutyCycleOffset -0.99
#define kSpeedModelFactor 0.041
#define kSpeedModelSpeedOffset 0.66

// Best params at 0.4 m/s
#define kP 10.0
#define kI 5.0
#define kD 0 //0.5

#define kPWMDutyCycleMin 0.0f
#define kPWMDutyCycleMax 1.0f

int32_t left_wheel_num_ticks;
int32_t right_wheel_num_ticks;

static void LeftEncoderIsr(TimerTicksType timer_ticks) {
  ++left_wheel_num_ticks;
}

static void RightEncoderIsr(TimerTicksType timer_ticks) {
  ++right_wheel_num_ticks;
}

void InitWheelSpeedControl() {
  left_wheel_num_ticks = 0;
  right_wheel_num_ticks = 0;
  AddEncoderIsrs(&LeftEncoderIsr, &RightEncoderIsr);
}

int32_t GetLeftWheelTickCount() {
  NO_ENCODER_IRQ {
    return left_wheel_num_ticks;
  }
  return 0;
}

int32_t GetRightWheelTickCount() {
  NO_ENCODER_IRQ {
    return right_wheel_num_ticks;
  }
  return 0;
}

WheelSpeedController::WheelSpeedController(
  WheelTickCountGetter * const wheel_tick_count_getter, 
  DutyCycleSetter * const duty_cycle_setter) 
  : PeriodicRunnable(kControlLoopPeriodSeconds), 
    wheel_tick_count_getter_(*ASSERT_NOT_NULL(wheel_tick_count_getter)), 
    duty_cycle_setter_(*ASSERT_NOT_NULL(duty_cycle_setter)), 
    last_run_seconds_(-1),
    average_wheel_speed_(0),
    is_turning_forward_(true),
    pid_(kP, kI, kD)
{
}

float WheelSpeedController::DutyCycleFromLinearSpeed(float meters_per_second) const {
  // The wheel speed model was extracted experimentally here:
  // https://docs.google.com/spreadsheets/d/1u54eKSRv8ef7i6d9K4IdrqnENu4u1ELPQnM7t2nn1fk
  // 
  // wheel_speed = max(0, kSpeedModelSpeedOffset - kSpeedModelFactor * exp(-(duty_cycle + kSpeedModelDutyCycleOffset) / kSpeedModelTimeConstant))
  // 
  // This function computes the inverse of said model for both forward and backward speeds
  // and imposes duty cycle limits.
  if (meters_per_second == 0) {
    // If speed is 0, force the duty cycle to 0 to save power. 
    return 0;
  }
  if (meters_per_second >= kSpeedModelSpeedOffset) {
    return 1.0f;
  }
  if (meters_per_second <= -kSpeedModelSpeedOffset) {
    return -1.0f;
  }
  const float offset = kSpeedModelTimeConstant * log(kSpeedModelFactor) - kSpeedModelDutyCycleOffset;
  float duty_cycle = offset - kSpeedModelTimeConstant * log(kSpeedModelSpeedOffset - fabsf(meters_per_second));
  duty_cycle = std::clamp(duty_cycle, kPWMDutyCycleMin, kPWMDutyCycleMax);
  if (meters_per_second < 0) {
    duty_cycle = -duty_cycle;
  }
  return duty_cycle;
}

void WheelSpeedController::SetLinearSpeed(float meters_per_second) {  
  time_start_ = GetTimerSeconds();
  num_wheel_ticks_start_ = wheel_tick_count_getter_();
  pid_.target(meters_per_second);
  // pid_.ResetIntegrator();
}

void WheelSpeedController::SetAngularSpeed(float radians_per_second) {
  SetLinearSpeed(radians_per_second * kWheelRadius);
}

void WheelSpeedController::RunAfterPeriod(TimerNanosType now_nanos, TimerNanosType nanos_since_last_call) {
  // Estimate wheel turn direction.
  // Assume the wheel is turning in the commanded direction because we cannot sense it.
  // If the target is zero, it does not work as proxy of the speed sign, so infer the 
  // sign from the previous pid output. Without this, when the target is zero, a speed
  // error can destabilize the control loop and make the robot drive backwards 
  // indefinitely.
  bool is_turning_forward = pid_.target() > 0 || (pid_.target() == 0 && pid_.output() >= 0);
  if (is_turning_forward != is_turning_forward_) {
    // The turn direction changed: reset the speed estimation.
    average_wheel_speed_ = 0;
  }
  is_turning_forward_ = is_turning_forward;

  // Estimate wheel speed.
  const float seconds_since_start = SecondsFromNanos(now_nanos) - time_start_;
  int num_encoder_ticks = wheel_tick_count_getter_() - num_wheel_ticks_start_;
  if (num_encoder_ticks > 0) {
    // Only update the speed estimate if get got any encoder ticks since the last change of
    // target speed. Otherwise, we'd get 0 after every change and the consequent control 
    // peak.
    float cur_avg_speed = kWheelRadius * kRadiansPerWheelTick * num_encoder_ticks / seconds_since_start;
    if (!is_turning_forward_) {
      // Assume the wheel is turning in the commanded direction because we cannot sense it.
      cur_avg_speed = -cur_avg_speed;
    }
    average_wheel_speed_ = cur_avg_speed;
  }

  // Update duty cycle with the speed estimate.
  const float pid_output = pid_.update(average_wheel_speed_, SecondsFromNanos(nanos_since_last_call));
  float speed_command = pid_.target() + pid_output;
  if ((is_turning_forward && speed_command < 0) || (!is_turning_forward && speed_command > 0)) {
    // Avoid speed commands opposite to the driving direction as that can make the wheel 
    // slip and hurt localization, making the error irrecoverable by the trajectory
    // controller.
    speed_command = 0;
  }
  const float duty_cycle = DutyCycleFromLinearSpeed(speed_command);
  duty_cycle_setter_(duty_cycle);

  // Serial.printf("t:%f v:%f pid:%f c:%f d:%f\n", pid_.target(), average_wheel_speed_, pid_output, speed_command, duty_cycle);
}

float WheelSpeedController::GetMaxLinearSpeed() const {
  const float offset = kSpeedModelTimeConstant * log(kSpeedModelFactor) - kSpeedModelDutyCycleOffset;
  return kSpeedModelSpeedOffset - exp((offset - kPWMDutyCycleMax) / kSpeedModelTimeConstant);
}

float WheelSpeedController::GetMaxAngularSpeed() const {
  return GetMaxLinearSpeed() / kWheelRadius;
}

float WheelSpeedController::GetMinLinearSpeed() const {
  return -GetMaxLinearSpeed();
}

float WheelSpeedController::GetMinAngularSpeed() const {
  return -GetMaxAngularSpeed();
}
