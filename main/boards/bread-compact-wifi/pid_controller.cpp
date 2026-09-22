#include "pid_controller.h"
#include <math.h>

void pid_reset(PIDController *pid){
    pid->prev_fb=0.0f;
    pid->integral=0.0f;
    pid->prev_output=0.0f;
}

void pid_init(PIDController *pid, float Kp,float Ki,float Kd,uint32_t dt_ms){
    pid->Kp=Kp;
    pid->Ki=Ki;
    pid->Kd=Kd;
    pid->out_min=-6680;
    pid->out_max=6680;
    pid->integral_min=pid->out_min*0.8f;
    pid->integral_max=pid->out_max*0.8f;
    pid->dt_ms=dt_ms;
    pid_reset(pid);
}



float pid_update(PIDController *pid, float setpoint, float feedback){
    float error=setpoint-feedback;
    float proportional=pid->Kp*error;
    float dt_sec=pid->dt_ms/1000.0f;
    float integral=pid->integral;
    integral+=error*dt_sec;
    if(integral>pid->integral_max) integral=pid->integral_max;
    else if(integral<pid->integral_min) integral=pid->integral_min;
    float integral_term=pid->Ki*integral;
    float derivative=(pid->prev_fb-feedback)/dt_sec;
    float derivative_term=derivative*pid->Kd;
    
    float output=proportional+integral_term+derivative_term;
    if(output>pid->out_max) output=pid->out_max;
    else if(output<pid->out_min) output=pid->out_min;
    if (output>=pid->out_max&&error>0){}
    else if (output<=pid->out_min&&error<0){}
    else {pid->integral=integral;}
    
    pid->prev_fb=feedback;
    pid->prev_output=output;
    return output;    
}
