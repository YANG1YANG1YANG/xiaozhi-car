#ifndef CAR_CONTROLLER_H_
#define CAR_CONTROLLER_H_

#include "mcp_server.h"
#include "driver/mcpwm.h"
#include "driver/gpio.h"
#include "driver/pcnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "cJSON.h"
#include "pid_controller.h"
#include <math.h>
#include <atomic>

#define COUNTER_RANGE 32767
#define OVERFLOW_THRESHOLD 20000
#define POSITION_KP 0.005f
#define STRAIGHT_DELTA_MAX_SPPED 2000.0f
#define RTN_MAX_SPEED 3340.0f
#define RTN_MIN_SPEED 300.0f
#define DT 0.010f
#define KP 5.0f
#define KI 8.0f
#define KD 0.0f
#define DT_MS 10
#define OUT_MIN -6680
#define OUT_MAX 6680




class CarController {

// ================================================================
// LAYER 1 : HARDWARE ABSTRACTION (硬件抽象层)
// 职责：仅包含 GPIO、MCPWM、PCNT 的直接操作，没有任何业务逻辑。
// ================================================================

private:
    gpio_num_t in1_a_, in2_a_, pwm_a_;
    gpio_num_t in1_b_, in2_b_, pwm_b_;
    gpio_num_t stby_pin_;
    gpio_num_t enc_a_pin_, enc_b_pin_;    // 左轮编码器
    gpio_num_t enc_a2_pin_, enc_b2_pin_;  // 右轮编码器

    //函数声明

    void motor_gpio_init();

    void motor_pwm_init();

    void encoder_init();

    void motor_set_speed(int motor, int speed);
    
    int16_t encoder_get_count(pcnt_unit_t unit);

    void set_standby(bool enable);





// ================================================================
// LAYER 2 : SOFTWARE ALGORITHM (软件算法层)
// ================================================================
//  PID 控制器 

private:

    PIDController pid_left_;
    PIDController pid_right_;

    //  速度相关（float 用于计算） 
    float target_speed_left_ = 0.0f;      // 目标速度（-6680~6680）
    float target_speed_right_ = 0.0f;
    float filtered_speed_left_ = 0.0f;    // 滤波后速度
    float filtered_speed_right_ = 0.0f;

    //  编码器累积（位置闭环） 
    int64_t left_cumul_ = 0;
    int64_t right_cumul_ = 0;
    int64_t target_pos_left_ = 0;
    int64_t target_pos_right_ = 0;

    //  上一次编码器值（用于计算增量） 
    int16_t last_count_left_ = 0;
    int16_t last_count_right_ = 0;

    int32_t current_delta_left_ = 0;
    int32_t current_delta_right_ = 0;

    //  PID 输出（转换后给硬件） 
    int output_duty_left_ = 0;    // -100 ~ 100
    int output_duty_right_ = 0;

    //  位置 PID 参数 
    float position_kp_ = POSITION_KP;
    float straight_delta_max_speed_ = STRAIGHT_DELTA_MAX_SPPED;
    float rtn_max_speed_ = RTN_MAX_SPEED;
    float rtn_min_speed_ = RTN_MIN_SPEED;


    //  接口函数 
    void set_target_speed(float left, float right);
    
    void pid_reset_both();
    
    void reset_all();

    //  位置 PID 算法（由状态机调用） 
    void position_pid_rtnhome();

    void position_pid_straight();
    
    //  滤波 + PID 执行（由 Task 调用） 
    void update_filter_and_pid();
    
    //  输出到硬件（由 Task 调用） 
    void apply_pid_output();
    
    // 强制停止
    void force_stop();



// ================================================================
// LAYER 3 : STATE MACHINE (状态机层)
// 职责：维护状态枚举、处理去抖延迟、决定状态跳转。
// 注意：这里不直接操作硬件，通过调用 LAYER 2 和 LAYER 1 的接口实现。
// ================================================================

public:
    // 暴露给外部的状态枚举（便于调试）
    enum class State {
        IDLE,           // 完全空闲，电机失能
        BEING_MOVED,    // 被外力推动中，电机失能
        WAIT_RETURN,    // 外力停止，等待 15 ticks 后归位
        RETURNING,      // 【独立顶级状态】执行位置归位（回原点）
        ACTIVE,         // 【独立顶级状态】执行用户主动指令（move_path）
        WAIT_IDLE       // 运动停止，等待 15 ticks 消除惯性
    };

private:
    State state_ = State::WAIT_IDLE;
    bool first_enter_ = true;   // 每个状态首次进入标志
    int wait_cnt_ = 0;          // 用于 WAIT_xxx 状态的计数器
    bool path_running_ = false;
    bool is_straight_=false;
    std::atomic<bool> force_stop_ {false};   // 外部强制停止标志（供 MCP 工具调用）
    
    //至于为什么需要atomic是因为esp32有两个core 可能读取旧core的值
    //还需要在编译的地方加上这个库！！！！

     // 状态机核心运行函数（定义在类内部）
    void run_state_machine();

    // 状态名转字符串（调试打印用）
    const char* state_to_string(State s);



// ================================================================
// LAYER 5 : TASK IMPLEMENTATION (放在类定义外部或内部)
// ================================================================


private:
    static TaskHandle_t motor_ctrl_task_handle_;  
   
    // 发送vofa数据
    void send_vofa_data();

    // 任务函数实现（可以直接在类内部定义，但 static 成员函数可以放在类外）
    static void motor_control_task(void* arg);
    


// ================================================================
// LAYER 4 : MCP TOOL INTERFACE (应用接口层)
// 职责：对外暴露 MCP 工具，解析 JSON，调用内部接口。
// 这些函数由构造函数中的 lambda 回调调用。
// ================================================================
public:

    //  构造函数（内部注册所有 MCP 工具） 
    CarController(
        gpio_num_t in1_a, gpio_num_t in2_a, gpio_num_t pwm_a,
        gpio_num_t in1_b, gpio_num_t in2_b, gpio_num_t pwm_b,
        gpio_num_t stby_pin,
        gpio_num_t enc_a, gpio_num_t enc_b,
        gpio_num_t enc_a2, gpio_num_t enc_b2);
    ~CarController();   

    // 立即停止
    void stop();

    // 设置速度 PID 参数
    void set_pid(float kp_l, float ki_l, float kd_l,
                 float kp_r, float ki_r, float kd_r);

    // 设置位置 PID 的 Kp
    void set_position_pid(float kp);

    // 执行多段路径（JSON 数组）
    bool start_path(const std::string& segments_json);

    //  析构函数 

};

#endif // CAR_CONTROLLER_H