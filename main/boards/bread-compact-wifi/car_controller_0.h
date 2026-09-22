#ifndef __CAR_CONTROLLER_H__
#define __CAR_CONTROLLER_H__

#include "mcp_server.h"
#include "driver/mcpwm.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"          // ★ 新增：定时器头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "pid_controller.h"

class CarController {
private:
    // 引脚成员变量
    gpio_num_t in1_a_, in2_a_, pwm_a_;
    gpio_num_t in1_b_, in2_b_, pwm_b_;
    gpio_num_t stby_pin_;

    // 速度状态
    int speed_left_ = 0;
    int speed_right_ = 0;

    // ★ 新增：定时运动相关成员
    esp_timer_handle_t move_timer_ = nullptr;   // 运动定时器
    bool is_moving_ = false;        
    bool force_stop_ = false;        
    float speed_factor_ = 30.0;             
    static void OnMoveTimerCallback(void* arg);

    //PID
    PIDController pid_left_;
    PIDController pid_right_;
    uint32_t pid_dt_ms_=10;
    float actual_speed_left_=0.0f;
    float actual_speed_right_=0.0f;

    // 设置单个电机速度（开环）
    void set_motor_speed(int motor, int speed) {
        if (speed > 100) speed = 100;
        if (speed < -100) speed = -100;

        int duty = abs(speed);
        if (motor == 0) { // 电机A
            if (speed >= 0) {
                gpio_set_level(in1_a_, 1);
                gpio_set_level(in2_a_, 0);
            } else {
                gpio_set_level(in1_a_, 0);
                gpio_set_level(in2_a_, 1);
            }
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, duty);
            mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
        } else { // 电机B
            if (speed >= 0) {
                gpio_set_level(in1_b_, 1);
                gpio_set_level(in2_b_, 0);
            } else {
                gpio_set_level(in1_b_, 0);
                gpio_set_level(in2_b_, 1);
            }
            mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, duty);
            mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
        }
    }

    void set_target_speed(int left, int right) {
        speed_left_ = left;
        speed_right_ = right;
        set_motor_speed(0, left);
        set_motor_speed(1, right);
    }

    void execute_timed_move(int left_speed, int right_speed, int duration_ms) {
        // 打断之前的运动
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

public:
    // 构造函数：传入所有引脚
    CarController(gpio_num_t in1_a, gpio_num_t in2_a, gpio_num_t pwm_a,
                  gpio_num_t in1_b, gpio_num_t in2_b, gpio_num_t pwm_b,
                  gpio_num_t stby_pin)
        : in1_a_(in1_a), in2_a_(in2_a), pwm_a_(pwm_a),
          in1_b_(in1_b), in2_b_(in2_b), pwm_b_(pwm_b),
          stby_pin_(stby_pin) {

        uint64_t pin_mask = (1ULL << in1_a_) | (1ULL << in2_a_) |
                            (1ULL << in1_b_) | (1ULL << in2_b_) |
                            (1ULL << stby_pin_);
        gpio_config_t io_conf = {
            .pin_bit_mask = pin_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        // 使能电机驱动
        gpio_set_level(stby_pin_, 1);

        // ---------- 2. MCPWM 初始化 ----------
        // 先短暂延时，让 GPIO 稳定
        vTaskDelay(pdMS_TO_TICKS(10));

        mcpwm_config_t pwm_config = {
            .frequency = 10000,
            .cmpr_a = 0,
            .cmpr_b = 0,
            .duty_mode = MCPWM_DUTY_MODE_0,
            .counter_mode = MCPWM_UP_COUNTER,
        };

        //初始化定时器0和1，并检查错误
        if (mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config) != ESP_OK) {
            ESP_LOGE("CarController", "MCPWM timer0 init failed");
            return;
        }
        if (mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config) != ESP_OK) {
            ESP_LOGE("CarController", "MCPWM timer1 init failed");
            return;
        }
        if (mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, pwm_a_) != ESP_OK) {
            ESP_LOGE("CarController", "MCPWM GPIO A init failed");
            return;
        }
        if (mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, pwm_b_) != ESP_OK) {
            ESP_LOGE("CarController", "MCPWM GPIO B init failed");
            return;
        }

        // 初始停止
        set_target_speed(0, 0);

        //PID
        pid_init(&pid_left_,  13.0f, 2.2f, 0.35f, 10);
        pid_init(&pid_right_, 13.0f, 2.2f, 0.35f, 10);

        // ---------- 3. 注册 MCP 工具 ----------
        auto& server = McpServer::GetInstance();

        // 原有的快速指令（保留，但为了不被定时运动干扰，需检查 is_moving_）
        // server.AddTool("car.forward", "Move forward", PropertyList(),
        //     [this](const PropertyList&) {
        //         if (is_moving_) {  // 如果正在定时运动，先取消
        //             if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
        //             is_moving_ = false;
        //         }
        //         set_target_speed(50, 50);
        //         return true;
        //     });

        // server.AddTool("car.backward", "Move backward", PropertyList(),
        //     [this](const PropertyList&) {
        //         if (is_moving_) {
        //             if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
        //             is_moving_ = false;
        //         }
        //         set_target_speed(-50, -50);
        //         return true;
        //     });

        // server.AddTool("car.turn_left", "Turn left in place", PropertyList(),
        //     [this](const PropertyList&) {
        //         if (is_moving_) {
        //             if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
        //             is_moving_ = false;
        //         }
        //         set_target_speed(-50, 50);
        //         return true;
        //     });

        // server.AddTool("car.turn_right", "Turn right in place", PropertyList(),
        //     [this](const PropertyList&) {
        //         if (is_moving_) {
        //             if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
        //             is_moving_ = false;
        //         }
        //         set_target_speed(50, -50);
        //         return true;
        //     });

        // ★ 修改：car.stop 现在会打断定时运动并停止电机
        server.AddTool("car.stop", "Stop the car immediately", PropertyList(),
            [this](const PropertyList&) {
                if (is_moving_) {
                    if (move_timer_ != nullptr) {
                        esp_timer_stop(move_timer_);
                    }
                    is_moving_ = false;
                }
                force_stop_ = true;
                set_target_speed(0, 0);
                //Application::GetInstance().StartListening();
                return true;
            });

        // ★ 新增：万能定时运动工具 —— 小智自己输入左右轮速度和持续时间
        server.AddTool("car.move_timed",
            "Control the car by independently setting left and right wheel speeds for a specific duration. "
            "This is the most flexible tool for turning, circling, and curved movements. "
            "Speed range: -100 to 100 (positive = forward, negative = backward). "
            "Duration is in milliseconds (1000 = 1 second). "
            "Don't use this tool if the user demands multiple segments of movement e.g turn right and then left."
            "Common actions and their parameter examples: "
            "'turn left on the spot' -> left_speed=-50, right_speed=50, duration_ms=1500; "
            "'turn right on the spot' -> left_speed=50, right_speed=-50, duration_ms=1500; "
            "'turn left while moving forward (arc)' -> left_speed=30, right_speed=60, duration_ms=2000; "
            "'turn right while moving forward (arc)' -> left_speed=60, right_speed=30, duration_ms=2000; "
            "'spin in a circle (360度原地旋转)' -> left_speed=-80, right_speed=80, duration_ms=3000; "
            "If the user does not specify a duration, use 2000ms as default. "
            "If the user does not specify speeds, use left_speed=40, right_speed=40 (straight forward) as default.",
            PropertyList({
                Property("left_speed", kPropertyTypeInteger, 40, -100, 100),
                Property("right_speed", kPropertyTypeInteger, 40, -100, 100),
                Property("duration_ms", kPropertyTypeInteger, 2000, 100, 30000)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                // ★ 直接调用封装好的内部函数，一行搞定 ★
                execute_timed_move(
                    props["left_speed"].value<int>(),
                    props["right_speed"].value<int>(),
                    props["duration_ms"].value<int>()
                );
                //Application::GetInstance().StartListening();
                return true;
            });

        server.AddTool("car.move_path",
            "Execute a sequence of movements continuously without stopping between segments. "
            "Each segment is defined by left_speed, right_speed, and duration_ms. "
            "This is ideal for S-curves, smooth turns, and complex choreographed paths. "
            "Example: S-curve path -> [{\"left\":30,\"right\":60,\"dur\":1000}, {\"left\":60,\"right\":30,\"dur\":1000}]",
            PropertyList({
                Property("segments", kPropertyTypeString)  // 传入 JSON 数组字符串
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string segments_json = props["segments"].value<std::string>();
                
                // 1. 解析 JSON 数组
                cJSON* root = cJSON_Parse(segments_json.c_str());
                if (root == nullptr || !cJSON_IsArray(root)) {
                    ESP_LOGE("CarController", "Invalid segments JSON");
                    return false;
                }
                
                // 2. 如果当前正在运动，先停止
                if (is_moving_) {
                    if (move_timer_ != nullptr) esp_timer_stop(move_timer_);
                    is_moving_ = false;
                }
                force_stop_ = false;
                
                // 3. 循环处理每一段
                int array_size = cJSON_GetArraySize(root);
                for (int i = 0; i < array_size; i++) {
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
                    
                    // 执行本段运动
                    set_target_speed(l, r);
                    ESP_LOGI("CarController", "Path segment %d: L=%d, R=%d, dur=%d ms", i, l, r, d);
                    
                    for(int j = 0; j < 5; j++){
                        if (force_stop_==true) return true;
                        vTaskDelay(pdMS_TO_TICKS(d/5));
                    }
                }
                
                // 4. 所有段执行完毕，停车
                set_target_speed(0, 0);
                cJSON_Delete(root);
                ESP_LOGI("CarController", "Path completed");
                // Application::GetInstance().StartListening();
                return true;
            });

        


        ESP_LOGI("CarController", "Car MCP tools registered (with timed movement).");
    }
};

// ★ 静态回调函数的实现
inline void CarController::OnMoveTimerCallback(void* arg) {
    CarController* self = static_cast<CarController*>(arg);
    // 时间到，立即停车
    self->set_target_speed(0, 0);
    self->is_moving_ = false;
    ESP_LOGI("CarController", "Timed movement finished (stopped by timer)");
}

#endif // __CAR_CONTROLLER_H__