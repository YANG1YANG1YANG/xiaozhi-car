#ifndef __CAR_CONTROLLER_H__
#define __CAR_CONTROLLER_H__

#include "mcp_server.h"
#include "driver/mcpwm.h"
#include "driver/gpio.h"
#include "driver/pcnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "pid_controller.h"
#include <math.h>

#define COUNTER_RANGE 32767
#define OVERFLOW_THRESHOLD 20000

class CarController {
private:
    // ---------- 引脚成员变量（构造函数传入） ----------
    gpio_num_t in1_a_, in2_a_, pwm_a_;
    gpio_num_t in1_b_, in2_b_, pwm_b_;
    gpio_num_t stby_pin_;
    gpio_num_t enc_a_pin_, enc_b_pin_;    // 左轮编码器
    gpio_num_t enc_a2_pin_, enc_b2_pin_;  // 右轮编码器

    // 速度状态
    int speed_left_ = 0;
    int speed_right_ = 0;

    // 定时运动相关
    esp_timer_handle_t move_timer_ = nullptr;
    bool force_stop_ = false;
    bool is_moving_=false;
    bool is_returning_=false,is_straight_=false;
    bool is_being_moved_=false;
    bool is_idle_=true;
    bool was_moving_=false,was_being_moved_=false,is_still_=false;
    bool first_enter_=true;

    // float speed_factor_ = 30.0;
    static void OnMoveTimerCallback(void* arg);


    // ---------- PID 控制 ----------
    PIDController pid_left_;
    PIDController pid_right_;
    float target_speed_left_ = 0.0f;
    float target_speed_right_ = 0.0f;
    int64_t left_cumul_=0,right_cumul_=0;
    int64_t target_pos_left_=0,target_pos_right_=0;
    float position_kp_=0.5f;
    float straight_max_speed_=2000.0f,rtn_max_speed_=3340.0f;

    static TaskHandle_t motor_ctrl_task_handle_;
    static CarController* instance_for_task_;



    void position_pid_rtnhome(void) {
        int64_t err_left = target_pos_left_ - left_cumul_;
        int64_t err_right = target_pos_right_ - right_cumul_;

        float spd_left = position_kp_ * err_left;
        float spd_right = position_kp_ * err_right;

        // 限幅
        if (spd_left > rtn_max_speed_) spd_left = rtn_max_speed_;
        if (spd_left < -rtn_max_speed_) spd_left = -rtn_max_speed_;
        if (spd_right > rtn_max_speed_) spd_right = rtn_max_speed_;
        if (spd_right < -rtn_max_speed_) spd_right = -rtn_max_speed_;

        target_speed_left_ = spd_left;
        target_speed_right_ = spd_right;
    }

    void position_pid_straight(void){
        int64_t err =  right_cumul_- left_cumul_;

        float delta_spd = position_kp_ * err;
        // 限幅
        
        if (delta_spd > straight_max_speed_) delta_spd = straight_max_speed_;
        if (delta_spd < -straight_max_speed_) delta_spd = -straight_max_speed_;


        target_speed_left_ = target_speed_left_ + delta_spd;
        target_speed_right_ = target_speed_right_ - delta_spd;
    }

    // ---------- 底层硬件驱动（使用成员引脚） ----------
    void motor_set_speed(int motor, int speed) {
        if (speed > 100) speed = 100;
        if (speed < -100) speed = -100;
        int duty = abs(speed);

        if (motor == 0) {
            gpio_set_level(in1_a_, speed >= 0 ? 1 : 0);
            gpio_set_level(in2_a_, speed >= 0 ? 0 : 1);
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, duty);
            mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
        } else {
            gpio_set_level(in1_b_, speed >= 0 ? 1 : 0);
            gpio_set_level(in2_b_, speed >= 0 ? 0 : 1);
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, duty);
            mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
        }
    }

    int16_t encoder_get_count(pcnt_unit_t unit) {
        int16_t count = 0;
        pcnt_get_counter_value(unit, &count);
        return count;
    }

    // ---------- 设置目标速度（百分比转实际值） ----------
    void set_target_speed(int left, int right) {
        // const float MAX_SPEED = 3340.0f; // 根据实际编码器标定调整
        target_speed_left_ = (float)left;
        target_speed_right_ = (float)right;
         // speed_left_ = (float)left;
        // speed_right_ = (float)right;
    }

    // ---------- 定时运动执行 ----------
    void execute_timed_move(int left_speed, int right_speed, int duration_ms) {
        if (is_moving_) {
            if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
            is_moving_ = false;
        }
        if (left_speed == 0 && right_speed == 0) {
            set_target_speed(0, 0);
            return;
        }
        set_target_speed(left_speed, right_speed);

        if (move_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = &CarController::OnMoveTimerCallback,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "car_move_timer",
                .skip_unhandled_events = false
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &move_timer_));
        }
        is_moving_ = true;
        ESP_ERROR_CHECK(esp_timer_start_once(move_timer_, duration_ms * 1000));
        ESP_LOGI("CarController", "Move: L=%d, R=%d, dur=%d ms", left_speed, right_speed, duration_ms);
    }

    // ---------- PID 控制任务（静态） ----------
    static void motor_control_task(void* arg) {
        CarController* self = (CarController*)arg;
        PIDController* pidL = &self->pid_left_;
        PIDController* pidR = &self->pid_right_;

        pid_reset(pidL);
        pid_reset(pidR);

        int16_t last_count1 = self->encoder_get_count(PCNT_UNIT_0);
        int16_t last_count2 = self->encoder_get_count(PCNT_UNIT_1);
        float filtered_speed1 = 0.0f, filtered_speed2 = 0.0f;
        TickType_t xLastWakeTime = xTaskGetTickCount();
        // const TickType_t xFrequency = pdMS_TO_TICKS(dt*1000);
        float dt = 0.010f;
        TickType_t xFrequency = pdMS_TO_TICKS(10); 

        int wait_cnt=0;
        static int print_cnt = 0;



        // const int COUNTER_RANGE = 32767;
        // const int OVERFLOW_THRESHOLD = 20000;

        while (1) {
            // if (fabsf(self->target_speed_left_) < 800 && fabsf(self->target_speed_right_) < 800) {
            //     dt = 0.030f;              
            //     xFrequency = pdMS_TO_TICKS(30);
            // } else {
            //     dt = 0.010f;              
            //     xFrequency = pdMS_TO_TICKS(10);
            // }
            // dt = 0.010f;              
            // xFrequency = pdMS_TO_TICKS(10);
            vTaskDelayUntil(&xLastWakeTime, xFrequency);

            
            int16_t count1 = self->encoder_get_count(PCNT_UNIT_0);
            int16_t count2 = self->encoder_get_count(PCNT_UNIT_1);
            
            int32_t delta1 = count1 - last_count1;
            int32_t delta2 = count2 - last_count2;
            if (delta1 > OVERFLOW_THRESHOLD) delta1 -= COUNTER_RANGE;
            else if (delta1 < -OVERFLOW_THRESHOLD) delta1 += COUNTER_RANGE;
            if (delta2 > OVERFLOW_THRESHOLD) delta2 -= COUNTER_RANGE;
            else if (delta2 < -OVERFLOW_THRESHOLD) delta2 += COUNTER_RANGE;
            // float alpha1 = (fabsf(self->target_speed_left_) < 800) ? 0.25f : 0.4f; 
            // float alpha2 = (fabsf(self->target_speed_right_) < 800) ? 0.25f : 0.4f; 
            float alpha1 = 0.4f;
            float alpha2 = 0.4f;
            float raw_speed1 = delta1 / dt;
            float raw_speed2 = delta2 / dt;
            filtered_speed1 = alpha1 * raw_speed1 + (1-alpha1) * filtered_speed1;
            filtered_speed2 = alpha2 * raw_speed2 + (1-alpha2) * filtered_speed2;

            last_count1 = count1;
            last_count2 = count2;

            float output1 = pid_update(pidL, self->target_speed_left_, filtered_speed1);
            float output2 = pid_update(pidR, self->target_speed_right_, filtered_speed2);

            int speed1 = (int)(output1 / 66.8f);
            int speed2 = (int)(output2 / 66.8f);

            self->left_cumul_+=delta1;
            self->right_cumul_+=delta2;


            //状态机的转换 纯判断没有实际操作
            if(self->is_idle_){
                self->is_moving_=false;
                self->is_returning_=false;
                self->is_straight_=false;
                self->is_being_moved_=false;
                if(self->was_being_moved_){
                    wait_cnt++;
                    if(wait_cnt>=15){
                        self->is_idle_=false;
                        self->was_being_moved_=false;
                        self->is_moving_=true;
                        self->is_returning_=true;
                        wait_cnt=0;
                        self->first_enter_=true;
                    }
                }
                else if(self->was_moving_){
                    wait_cnt++;
                    if(wait_cnt>=15){
                        self->is_idle_=false;
                        self->was_moving_=false;
                        self->is_still_=true;
                        wait_cnt=0;
                        self->first_enter_=true;
                    }
                }
                else if(self->is_still_){
                    if(delta1!=0 || delta2!=0){
                        self->is_idle_=false;
                        self->is_still_=false;
                        self->is_being_moved_=true;
                        self->first_enter_=true;
                    }
                }
            }
            if(self->is_moving_){
                self->is_being_moved_=false;
                self->is_idle_=false;
                self->was_moving_=false;
                self->was_being_moved_=false;
                self->is_still_=false;
                if(fabsf(self->target_speed_left_) < 0.5f && fabsf(self->target_speed_right_) < 0.5f){
                    self->is_moving_=false;
                    self->is_returning_=false;
                    self->is_straight_=false;
                    self->is_idle_=true;
                    self->is_still_=true;
                    self->first_enter_=true;
                }   
                else if(self->is_returning_==true && fabsf(self->left_cumul_)<=10 && fabsf(self->right_cumul_)<=10){
                    self->is_moving_=false;
                    self->is_returning_=false;
                    self->is_idle_=true;
                    self->is_still_=true;
                    self->first_enter_=true;
                }
            }
            if(self->is_being_moved_){
                self->is_moving_=false;
                self->is_returning_=false;
                self->is_straight_=false;
                self->is_idle_=false;
                self->was_moving_=false;
                self->was_being_moved_=false;
                self->is_still_=false;
                if(delta1==0 && delta2==0){
                    self->is_being_moved_=false;
                    self->is_idle_=true;
                    self->was_being_moved_=true;
                    self->first_enter_=true;
                }
            }

            //根据状态操作硬件

            if(self->is_idle_){
                if(self->first_enter_){
                    gpio_set_level(self->stby_pin_,0);
                    filtered_speed1 = 0.0f; 
                    filtered_speed2 = 0.0f;
                    self->left_cumul_=0;
                    self->right_cumul_=0;
                    pid_reset(pidL);
                    pid_reset(pidR);
                    self->first_enter_=false;
                }
            }
            else if(self->is_being_moved_){
                if(self->first_enter_){
                    gpio_set_level(self->stby_pin_,0);
                    self->first_enter_=false;
                }
            }
            else if(self->is_moving_){
                gpio_set_level(self->stby_pin_,1);
                if(self->is_returning_){
                    self->position_pid_rtnhome();
                }
                else if(self->is_straight_){
                    self->position_pid_straight();
                }
                self->motor_set_speed(1, speed1);
                self->motor_set_speed(0, speed2);               
            }

            //打印

            if (++print_cnt % 50 == 0) {
                ESP_LOGI("DEBUG", "--- State ---");
                ESP_LOGI("DEBUG", "is_moving=%d, is_being=%d, is_return=%d", 
                        self->is_moving_, self->is_being_moved_, self->is_returning_);
                ESP_LOGI("DEBUG", "target L=%.1f R=%.1f", self->target_speed_left_, self->target_speed_right_);
                ESP_LOGI("DEBUG", "cumul L=%lld R=%lld", self->left_cumul_, self->right_cumul_);
                ESP_LOGI("DEBUG", "speed1=%d speed2=%d, delta1=%d delta2=%d", speed1, speed2, delta1, delta2);
                ESP_LOGI("DEBUG", "stby=%d", gpio_get_level(self->stby_pin_));
            }
        }
    }

    // ---------- 硬件初始化（使用成员引脚） ----------
    void motor_gpio_init() {
        gpio_set_direction(stby_pin_, GPIO_MODE_OUTPUT);
        gpio_set_level(stby_pin_, 1);
        gpio_set_direction(in1_a_, GPIO_MODE_OUTPUT);
        gpio_set_direction(in2_a_, GPIO_MODE_OUTPUT);
        gpio_set_direction(in1_b_, GPIO_MODE_OUTPUT);
        gpio_set_direction(in2_b_, GPIO_MODE_OUTPUT);
    }

    void motor_pwm_init() {
        mcpwm_config_t pwm_config = {
            .frequency = 10000,
            .cmpr_a = 0,
            .cmpr_b = 0,
            .duty_mode = MCPWM_DUTY_MODE_0,
            .counter_mode = MCPWM_UP_COUNTER,
        };
        mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
        mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config);
        mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, pwm_a_);
        mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, pwm_b_);
    }

    void encoder_init() {
        // 左编码器 (PCNT_UNIT_0)
        pcnt_config_t pcnt_a;
        memset(&pcnt_a, 0, sizeof(pcnt_a));  // 清零
        pcnt_a.pulse_gpio_num = enc_a_pin_;
        pcnt_a.ctrl_gpio_num = enc_b_pin_;
        pcnt_a.lctrl_mode = PCNT_MODE_KEEP;
        pcnt_a.hctrl_mode = PCNT_MODE_REVERSE;
        pcnt_a.pos_mode = PCNT_COUNT_INC;
        pcnt_a.neg_mode = PCNT_COUNT_DEC;
        pcnt_a.counter_h_lim = 32767;
        pcnt_a.counter_l_lim = -32768;
        pcnt_a.unit = PCNT_UNIT_0;
        pcnt_a.channel = PCNT_CHANNEL_0;
        pcnt_unit_config(&pcnt_a);
        pcnt_counter_clear(PCNT_UNIT_0);
        pcnt_counter_resume(PCNT_UNIT_0);

        // 右编码器 (PCNT_UNIT_1)
        pcnt_config_t pcnt_b;
        memset(&pcnt_b, 0, sizeof(pcnt_b));
        pcnt_b.pulse_gpio_num = enc_a2_pin_;
        pcnt_b.ctrl_gpio_num = enc_b2_pin_;
        pcnt_b.lctrl_mode = PCNT_MODE_KEEP;
        pcnt_b.hctrl_mode = PCNT_MODE_REVERSE;
        pcnt_b.pos_mode = PCNT_COUNT_INC;
        pcnt_b.neg_mode = PCNT_COUNT_DEC;
        pcnt_b.counter_h_lim = 32767;
        pcnt_b.counter_l_lim = -32768;
        pcnt_b.unit = PCNT_UNIT_1;
        pcnt_b.channel = PCNT_CHANNEL_0;
        pcnt_unit_config(&pcnt_b);
        pcnt_counter_clear(PCNT_UNIT_1);
        pcnt_counter_resume(PCNT_UNIT_1);
    }
public:
    // 构造函数：电机引脚 + 编码器引脚（全部可配置）
    CarController(
        gpio_num_t in1_a, gpio_num_t in2_a, gpio_num_t pwm_a,
        gpio_num_t in1_b, gpio_num_t in2_b, gpio_num_t pwm_b,
        gpio_num_t stby_pin,
        gpio_num_t enc_a, gpio_num_t enc_b,    // 左轮
        gpio_num_t enc_a2, gpio_num_t enc_b2)  // 右轮
        : in1_a_(in1_a), in2_a_(in2_a), pwm_a_(pwm_a),
          in1_b_(in1_b), in2_b_(in2_b), pwm_b_(pwm_b),
          stby_pin_(stby_pin),
          enc_a_pin_(enc_a), enc_b_pin_(enc_b),
          enc_a2_pin_(enc_a2), enc_b2_pin_(enc_b2) {

        // 1. 硬件初始化
        motor_gpio_init();
        motor_pwm_init();
        encoder_init();

        // 2. PID 初始化
        pid_init(&pid_left_,  10.0f, 0.0f, 0.05f, 10);
        pid_init(&pid_right_, 10.0f, 0.0f, 0.05f, 10);
        pid_left_.out_min = -6680;
        pid_left_.out_max = 6680;
        pid_right_.out_min = -6680;
        pid_right_.out_max = 6680;

        // 3. 创建 PID 任务
        instance_for_task_ = this;
        xTaskCreate(motor_control_task, "MotorPID", 4096, this, 5, &motor_ctrl_task_handle_);
        
        // 4. 初始停止
        set_target_speed(0, 0);
        

        // 5. 注册 MCP 工具
        auto& server = McpServer::GetInstance();




        server.AddTool("car.set_speed_pid",
            "Set PID parameters for speed. All values must be **integers** representing 100 times the actual value. For example, to set Kp=8.0, send 800; for Ki=1.0, send 100. "
            "Values for the pid are multiplied by 100 (e.g., 800 means 8.00). "
            "Example: left_kp=800 (8.00), left_ki=100 (1.00), left_kd=0",
            PropertyList({
                Property("left_kp", kPropertyTypeInteger, 800, 0, 10000),
                Property("left_ki", kPropertyTypeInteger, 100, 0, 10000),
                Property("left_kd", kPropertyTypeInteger, 0, 0, 10000),
                Property("right_kp", kPropertyTypeInteger, 800, 0, 10000),
                Property("right_ki", kPropertyTypeInteger, 100, 0, 10000),
                Property("right_kd", kPropertyTypeInteger, 0, 0, 10000),
            }),
            [this](const PropertyList& props) -> ReturnValue {
                float lkp = props["left_kp"].value<int>() / 100.0f;
                float lki = props["left_ki"].value<int>() / 100.0f;
                float lkd = props["left_kd"].value<int>() / 100.0f;
                float rkp = props["right_kp"].value<int>() / 100.0f;
                float rki = props["right_ki"].value<int>() / 100.0f;
                float rkd = props["right_kd"].value<int>() / 100.0f;
                this->SetPID(lkp, lki, lkd, rkp, rki, rkd);
                return true;
            });
        server.AddTool("car.set_position_pid",
            "Set PID parameters for position. All values must be **integers** representing 100 times the actual value. For example, to set Kp=8.0, send 800; for Ki=1.0, send 100. "
            "Values for the pid are multiplied by 100 (e.g., 10 means 0.10). "
            "Example: position_kp=10(0.1)",
            PropertyList({
                Property("position_kp", kPropertyTypeInteger, 100, 0, 10000),
            }),
            [this](const PropertyList& props) -> ReturnValue {
                position_kp_=props["position_kp"].value<int>()/100.0f;
                return true;
            });

    
        server.AddTool("car.stop", "Stop the car immediately", PropertyList(),
            [this](const PropertyList&) {
                if (is_moving_) {
                    if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
                }
                force_stop_ = true;
                set_target_speed(0, 0);
                left_cumul_=0;
                right_cumul_=0;
                is_idle_=true;
                was_moving_=true;
                is_moving_=false;
                is_returning_=false;
                is_straight_=false;
                return true;
            });


        

        // server.AddTool("car.move_timed",
        //     "Control the car by independently setting left and right wheel speeds for a specific duration. "
        //     "This is the most flexible tool for turning, circling, and curved movements. "
        //     "Speed range: -6680 to 6680 (positive = forward, negative = backward). "
        //     "Duration is in milliseconds (1000 = 1 second). "
        //     "Don't use this tool if the user demands multiple segments of movement e.g turn right and then left."
        //     "Common actions and their parameter examples: "
        //     "'turn left on the spot' -> left_speed=-3340, right_speed=3340, duration_ms=1500; "
        //     "'turn right on the spot' -> left_speed=3340, right_speed=-3340, duration_ms=1500; "
        //     "'turn left while moving forward (arc)' -> left_speed=3340, right_speed=6680, duration_ms=2000; "
        //     "'turn right while moving forward (arc)' -> left_speed=6680, right_speed=3340, duration_ms=2000; "
        //     "'spin in a circle (360度原地旋转)' -> left_speed=-3340, right_speed=3340, duration_ms=3000; "
        //     "If the user does not specify a duration, use 2000ms as default. "
        //     "If the user does not specify speeds, use left_speed=3340, right_speed=3340 (straight forward) as default.",
        //     PropertyList({
        //         Property("left_speed", kPropertyTypeInteger, 40, -6680, 6680),
        //         Property("right_speed", kPropertyTypeInteger, 40, -6680, 6680),
        //         Property("duration_ms", kPropertyTypeInteger, 2000, 100, 30000)
        //     }),
        //     [this](const PropertyList& props) -> ReturnValue {
        //         execute_timed_move(
        //             props["left_speed"].value<int>(),
        //             props["right_speed"].value<int>(),
        //             props["duration_ms"].value<int>()
        //         );
        //         return true;
        //     });

        server.AddTool("car.move_path",
            "Execute one or more continuous movement segments without stopping between them. "
            "Each segment is an object with left_speed, right_speed, duration_ms. "
            "The 'segments' parameter must be a JSON array string, e.g., "
            "'[{\"left\":3340,\"right\":6680,\"dur\":1000}, {\"left\":6680,\"right\":3340,\"dur\":1000}]'. "
            "Speed range: -6680 to 6680 (positive forward, negative backward). "
            "Duration in milliseconds. "
            "Common single segment: forward -> left=3340, right=3340, dur=2000; "
            "turn right on spot -> left=3340, right=-3340, dur=1500. "
            "turn left on spot -> left=-3340, right=3340, dur=1500; "
            "backward -> left=-3340, right=-3340, dur=2000; "
            "If duration not specified, use 2000ms; if speeds not specified, use 3340 for both."
            "You don't need to multply the value by 100 here",
            PropertyList({
                Property("segments", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string segments_json = props["segments"].value<std::string>();
                cJSON* root = cJSON_Parse(segments_json.c_str());
                if (root == nullptr || !cJSON_IsArray(root)) {
                    ESP_LOGE("CarController", "Invalid segments JSON");
                    return false;
                }
                if (is_moving_) {
                    if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
                    is_moving_ = false;
                }
                force_stop_ = false;
                int array_size = cJSON_GetArraySize(root);
                is_moving_=true;
                for (int i = 0; i < array_size; i++) {
                    is_straight_=false;
                    left_cumul_=0;
                    right_cumul_=0;
                    cJSON* seg = cJSON_GetArrayItem(root, i);
                    if (!cJSON_IsObject(seg)) continue;
                    cJSON* left = cJSON_GetObjectItem(seg, "left");
                    cJSON* right = cJSON_GetObjectItem(seg, "right");
                    cJSON* dur = cJSON_GetObjectItem(seg, "dur");
                    if (!cJSON_IsNumber(left) || !cJSON_IsNumber(right) || !cJSON_IsNumber(dur)) {
                        ESP_LOGW("CarController", "Skipping invalid segment %d", i);
                        continue;
                    }
                    int l = left->valueint;
                    int r = right->valueint;
                    int d = dur->valueint;
                    if(l==r) is_straight_=true;
                    set_target_speed(l, r);
                    ESP_LOGI("CarController", "Path segment %d: L=%d, R=%d, dur=%d ms", i, l, r, d);
                    for (int j = 0; j < 5; j++) {
                        if (force_stop_) {
                            left_cumul_=0;
                            right_cumul_=0;
                            is_moving_=false;
                            is_straight_=false;
                            is_idle_=true;
                            was_moving_=true;
                            first_enter_=true;
                            return true;
                        };
                        vTaskDelay(pdMS_TO_TICKS(d / 5));
                    }
                    // pid_init(&pid_left_,  25.0f, 8.0f, 0.0f, dt*1000);
                    // pid_init(&pid_right_, 25.0f, 8.0f, 0.0f, dt*1000);
                    // pid_reset(&pid_left_);
                    // pid_reset(&pid_right_);
                }
                set_target_speed(0, 0);
                cJSON_Delete(root);
                ESP_LOGI("CarController", "Path completed");

                left_cumul_=0;
                right_cumul_=0;
                is_moving_=false;
                is_straight_=false;
                is_idle_=true;
                was_moving_=true;
                first_enter_=true;

                return true;
            });

        ESP_LOGI("CarController", "Car MCP tools registered (with PID closed-loop control).");
    }

    void SetPID(float kp_l, float ki_l, float kd_l, float kp_r, float ki_r, float kd_r) {
            pid_left_.Kp = kp_l;
            pid_left_.Ki = ki_l;
            pid_left_.Kd = kd_l;
            pid_right_.Kp = kp_r;
            pid_right_.Ki = ki_r;
            pid_right_.Kd = kd_r;
            // 重置积分项，避免突变
            pid_reset(&pid_left_);
            pid_reset(&pid_right_);
            ESP_LOGI("CarController", "PID updated: L(%.2f,%.2f,%.2f) R(%.2f,%.2f,%.2f)",
                    kp_l, ki_l, kd_l, kp_r, ki_r, kd_r);
    }
    ~CarController() {
        if (move_timer_ != nullptr) {
            esp_timer_stop(move_timer_);
            esp_timer_delete(move_timer_);
        }
        if (motor_ctrl_task_handle_ != nullptr) {
            vTaskDelete(motor_ctrl_task_handle_);
        }
    }
};

// 静态成员初始化
TaskHandle_t CarController::motor_ctrl_task_handle_ = nullptr;
CarController* CarController::instance_for_task_ = nullptr;

// 定时器回调
inline void CarController::OnMoveTimerCallback(void* arg) {
    CarController* self = static_cast<CarController*>(arg);
    self->set_target_speed(0, 0);
    self->is_moving_ = false;
    ESP_LOGI("CarController", "Timed movement finished");
}

#endif // __CAR_CONTROLLER_H__