#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

#include <stdint.h>




typedef struct{
    float Kp;
    float Ki;
    float Kd;

    float out_min;
    float out_max;
    float integral_min;
    float integral_max;

    float prev_fb;
    float integral;
    float prev_output; 

    uint32_t dt_ms; // 控制周期 
}PIDController;

void pid_init(PIDController *pid, float Kp,float Ki,float Kd,uint32_t dt_ms);

void pid_reset(PIDController *pid);

float pid_update(PIDController *pid, float setpoint, float feedback);

#endif

