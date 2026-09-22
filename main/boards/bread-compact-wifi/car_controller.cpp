#include "car_controller.h"



TaskHandle_t CarController::motor_ctrl_task_handle_ = nullptr;


// ================================================================
// LAYER 1 : HARDWARE ABSTRACTION (硬件抽象层)
// 职责：仅包含 GPIO、MCPWM、PCNT 的直接操作，没有任何业务逻辑。
// ================================================================

void CarController::motor_gpio_init() {
    gpio_set_direction(stby_pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(stby_pin_, 1);
    gpio_set_direction(in1_a_, GPIO_MODE_OUTPUT);
    gpio_set_direction(in2_a_, GPIO_MODE_OUTPUT);
    gpio_set_direction(in1_b_, GPIO_MODE_OUTPUT);
    gpio_set_direction(in2_b_, GPIO_MODE_OUTPUT);
}

void CarController::motor_pwm_init() {
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

void CarController::encoder_init() {
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
   
    // 加上下面的变成四倍频 目前是两倍频
    // -------- Channel 1: pulse=B相, ctrl=A相 【四倍频必须！】 --------
    // pcnt_ch1.pulse_gpio_num = gpio_b;
    // pcnt_ch1.ctrl_gpio_num  = gpio_a;
    // pcnt_ch1.unit = unit;
    // pcnt_ch1.channel = PCNT_CHANNEL_1;

    // pcnt_ch1.lctrl_mode = PCNT_MODE_KEEP;
    // pcnt_ch1.hctrl_mode = PCNT_MODE_REVERSE;
    // pcnt_ch1.pos_mode = PCNT_COUNT_INC;
    // pcnt_ch1.neg_mode = PCNT_COUNT_DEC;

    // pcnt_ch1.counter_h_lim = 32767;
    // pcnt_ch1.counter_l_lim = -32768;
    // pcnt_unit_config(&pcnt_ch1);

    // pcnt_counter_clear(unit);
    // pcnt_counter_resume(unit);
}

void CarController::motor_set_speed(int motor, int speed) {
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

int16_t CarController::encoder_get_count(pcnt_unit_t unit) {
    int16_t count = 0;
    pcnt_get_counter_value(unit, &count);
    return count;
}

void CarController::set_standby(bool enable){
    if(enable){
        gpio_set_level(stby_pin_,1);
    } 
    else{
        gpio_set_level(stby_pin_,0);
    }   
}

// ================================================================
// LAYER 2 : SOFTWARE ALGORITHM (软件算法层)
// ================================================================
// ---------- PID 控制器 ----------


// ---------- 接口函数 ----------
void CarController::set_target_speed(float left, float right) {
    if(fabs(left)<=5.0f&&fabs(right)<=5.0f){
        target_speed_left_=0.0f;
        target_speed_right_=0.0f;
    }
    else{
        target_speed_left_ = left;
        target_speed_right_ = right;
    }
}

void CarController::pid_reset_both() {
    pid_reset(&pid_left_);
    pid_reset(&pid_right_);
}
void CarController::reset_all(){
    target_speed_left_=0.0f;
    target_speed_right_=0.0f;
    filtered_speed_left_=0.0f;
    filtered_speed_right_=0.0f;
    left_cumul_=0;
    right_cumul_=0;
    output_duty_left_=0;
    output_duty_right_=0;
    pid_reset_both();
}

// ---------- 位置 PID 算法（由状态机调用） ----------
void CarController::position_pid_rtnhome() {
    // int64_t err_left = target_pos_left_ - left_cumul_;
    // int64_t err_right = target_pos_right_ - right_cumul_;

    // float spd_left = position_kp_ * err_left;
    // float spd_right = position_kp_ * err_right;
    
    int64_t err_left = target_pos_left_ - left_cumul_;
    int64_t err_right = target_pos_right_ - right_cumul_;

    float spd_left = 5.0 * err_left;
    float spd_right = 5.0 * err_right;

    // 限幅
    if (spd_left > rtn_max_speed_) spd_left = rtn_max_speed_;
    if (spd_left < -rtn_max_speed_) spd_left = -rtn_max_speed_;
    if (spd_right > rtn_max_speed_) spd_right = rtn_max_speed_;
    if (spd_right < -rtn_max_speed_) spd_right = -rtn_max_speed_;

    if (spd_left < rtn_min_speed_ && spd_left>0) spd_left = rtn_min_speed_;
    if (spd_left > -rtn_min_speed_ && spd_left<0) spd_left = -rtn_min_speed_;
    if (spd_right < rtn_min_speed_ && spd_right>0) spd_right = rtn_min_speed_;
    if (spd_right > -rtn_min_speed_ && spd_right<0) spd_right = -rtn_min_speed_;

    target_speed_left_ = spd_left;
    target_speed_right_ = spd_right;
}

void CarController::position_pid_straight() {
    int64_t err = right_cumul_ - left_cumul_;
    float delta_spd = position_kp_ * err;

    if (delta_spd > straight_delta_max_speed_) delta_spd = straight_delta_max_speed_;
    if (delta_spd < -straight_delta_max_speed_) delta_spd = -straight_delta_max_speed_;

    target_speed_left_ += delta_spd;
    target_speed_right_ -= delta_spd;
}

// ---------- 滤波 + PID 执行（由 Task 调用） ----------
void CarController::update_filter_and_pid() {
    const float dt = 0.010f;  // 固定 10ms

    // 1. 硬件读取（原本在 Task 里做的，现在搬进来）
    int16_t countL = encoder_get_count(PCNT_UNIT_0);
    int16_t countR = encoder_get_count(PCNT_UNIT_1);

    int32_t deltaL = countL - last_count_left_;
    int32_t deltaR = countR - last_count_right_;
    
    // 2. 溢出处理（原 Task 中的逻辑）
    if (deltaL > OVERFLOW_THRESHOLD) deltaL -= COUNTER_RANGE;
    else if (deltaL < -OVERFLOW_THRESHOLD) deltaL += COUNTER_RANGE;
    if (deltaR > OVERFLOW_THRESHOLD) deltaR -= COUNTER_RANGE;
    else if (deltaR < -OVERFLOW_THRESHOLD) deltaR += COUNTER_RANGE;

    // 3. 更新上一次读数（供下次使用）
    last_count_left_ = countL;
    last_count_right_ = countR;

    // ESP_LOGI("ENC", "L=%d R=%d", countL, countR);

    // 4. 计算原始速度、滤波（原逻辑不变）
    float raw_speed_left = deltaL / dt;
    float raw_speed_right = deltaR / dt;

    const float alpha = 0.7f;
    filtered_speed_left_ = alpha * raw_speed_left + (1.0f - alpha) * filtered_speed_left_;
    filtered_speed_right_ = alpha * raw_speed_right + (1.0f - alpha) * filtered_speed_right_;

    // 5. PID 计算（原逻辑不变）
    float output_left = pid_update(&pid_left_, target_speed_left_, filtered_speed_left_);
    float output_right = pid_update(&pid_right_, target_speed_right_, filtered_speed_right_);

    // 6. 转换为占空比输出（原逻辑不变）
    output_duty_left_ = (int)(output_left / 66.8f);
    output_duty_right_ = (int)(output_right / 66.8f);

    if (output_duty_left_ > 100) output_duty_left_ = 100;
    if (output_duty_left_ < -100) output_duty_left_ = -100;
    if (output_duty_right_ > 100) output_duty_right_ = 100;
    if (output_duty_right_ < -100) output_duty_right_ = -100;

    // 7. 累积脉冲（供位置 PID 使用）
    left_cumul_ += deltaL;
    right_cumul_ += deltaR;
    current_delta_left_ = deltaL;
    current_delta_right_ = deltaR;
}

// ---------- 输出到硬件（由 Task 调用） ----------
void CarController::apply_pid_output() {
    // 注意：motor_set_speed 的第一个参数是 motor 编号
    // 0 = 左轮，1 = 右轮（根据你的硬件定义）
    if(fabs(output_duty_left_)<5) output_duty_left_=0; 
    if(fabs(output_duty_right_)<5) output_duty_right_=0; 
    //左右轮硬件反了
    motor_set_speed(1, output_duty_left_);   // 左轮
    motor_set_speed(0, output_duty_right_);  // 右轮
}

void CarController::force_stop(){
    set_target_speed(0,0);
}



// ================================================================
// LAYER 3 : STATE MACHINE (状态机层)
// 职责：维护状态枚举、处理去抖延迟、决定状态跳转。
// 注意：这里不直接操作硬件，通过调用 LAYER 2 和 LAYER 1 的接口实现。
// ================================================================


void CarController::run_state_machine() {
    bool is_delta = (fabs(current_delta_left_) != 0 || fabs(current_delta_right_) != 0);

    if(force_stop_){
        force_stop();
        wait_cnt_=0;
        is_straight_=false;
        path_running_=false;
        first_enter_=true;
        state_=State::WAIT_IDLE;
        force_stop_=false;
    }

    switch (state_) {
        case State::IDLE:
            if (first_enter_) {
                set_standby(false);
                // pid_reset_both();
                reset_all();
                first_enter_ = false;
            }
            if (is_delta) {
                state_ = State::BEING_MOVED;
                first_enter_ = true;
            }
            break;

        case State::BEING_MOVED:
            if (first_enter_) {
                set_standby(false);
                first_enter_ = false;
            }
            if (!is_delta) {
                state_ = State::WAIT_RETURN;
                wait_cnt_ = 0;
                first_enter_ = true;
            }
            break;

        case State::WAIT_RETURN:
            if (first_enter_) {
                set_standby(false);
                first_enter_ = false;
            }
            if (is_delta) {
                state_ = State::BEING_MOVED;
                first_enter_ = true;
                wait_cnt_=0;
            } else if (++wait_cnt_ >= 50) {
                state_ = State::RETURNING;
                target_pos_left_ = 0;
                target_pos_right_ = 0;
                first_enter_ = true;
                wait_cnt_ = 0;
            }
            break;

        case State::RETURNING:
            if (first_enter_) {
                set_standby(true);
                pid_reset_both();
                first_enter_ = false;
            }
            position_pid_rtnhome();

            if ((abs(left_cumul_) <= 15 && abs(right_cumul_) <= 15)) {
                state_ = State::WAIT_IDLE;
                wait_cnt_ = 0;
                first_enter_ = true;
                set_standby(false);
                force_stop_ = false;
            }
            break;

        case State::ACTIVE:
            if (first_enter_) {
                set_standby(true);
                pid_reset_both();
                first_enter_ = false;
            }
            // 若需要直线纠偏，可在此处调用 position_pid_straight()
            if(!path_running_){
                if ((fabs(target_speed_left_) < 0.5f && fabs(target_speed_right_) < 0.5f)) {
                    state_ = State::WAIT_IDLE;
                    first_enter_ = true;
                    set_standby(false);
                }
            }
            else{
                if(is_straight_){
                    position_pid_straight();
                }
            }
            break;

        case State::WAIT_IDLE:
            if (first_enter_) {
                first_enter_ = false;
                wait_cnt_=0;
            }
            // if (!force_stop_&&is_delta) {
            //     state_ = State::BEING_MOVED;
            //     first_enter_ = true;
            // } else 
            if (++wait_cnt_ >= 100) {
                state_ = State::IDLE;
                reset_all();
                first_enter_ = true;
            }
            break;
    }
}

    // 状态名转字符串（调试打印用）
const char* CarController::state_to_string(State s) {
    switch (s) {
        case State::IDLE:         return "IDLE";
        case State::BEING_MOVED:  return "BEING_MOVED";
        case State::WAIT_RETURN:  return "WAIT_RETURN";
        case State::RETURNING:    return "RETURNING";
        case State::ACTIVE:       return "ACTIVE";
        case State::WAIT_IDLE:    return "WAIT_IDLE";
        default:                  return "UNKNOWN";
    }
}









// ================================================================
// LAYER 5 : TASK IMPLEMENTATION (放在类定义外部或内部)
// ================================================================


void CarController::send_vofa_data() {
// 使用 printf 发送 CSV 格式数据
// 注意：printf 默认输出到 UART0（与 ESP_LOG 同一个串口）
// 如果日志干扰，可以单独配置一个 UART，但这里先用 printf
    printf("%.2f,%.2f,%.2f,%.2f\r\n",
        target_speed_left_, filtered_speed_left_,
        target_speed_right_, filtered_speed_right_);
}

// 任务函数实现（可以直接在类内部定义，但 static 成员函数可以放在类外）
void CarController::motor_control_task(void* arg) {
    CarController* self = static_cast<CarController*>(arg);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const float dt = 0.010f;  // 10ms

    // 初始化上一次编码器值
    self->last_count_left_ = self->encoder_get_count(PCNT_UNIT_0);
    self->last_count_right_ = self->encoder_get_count(PCNT_UNIT_1);

    static int print_cnt = 0;
    static int vofa_cnt = 0;
    while (1) {
        // 精确延时 10ms
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));


        // ========== 1. 执行软件算法（LAYER 2） ==========
        // 滤波 + 速度PID，计算结果存入 output_duty_left_/right_
        self->update_filter_and_pid();

        // ========== 2. 运行状态机（LAYER 3） ==========
        // 状态机内部会根据 delta 和当前状态决定跳转，并可能调用 LAYER 2 的位置PID
        self->run_state_machine();


        // ========== 3. 输出到硬件（LAYER 1） ==========
        self->apply_pid_output();   // 调用 motor_set_speed

        
        // ========== 4. 调试打印（可选） ==========
        if (++vofa_cnt >= 5) {
            vofa_cnt = 0;
            self->send_vofa_data();
        }

        if (++print_cnt % 50 == 0) {
            ESP_LOGI("CarCtrl", 
                    "State=%s, tL=%.1f tR=%.1f, dutyL=%d dutyR=%d, cumL=%d cumR=%d",
                    self->state_to_string(self->state_),
                    self->target_speed_left_, self->target_speed_right_,
                    self->output_duty_left_, self->output_duty_right_,
                    (int)self->left_cumul_, (int)self->right_cumul_);
        }
    }
}


// ================================================================
// LAYER 4 : MCP TOOL INTERFACE (应用接口层)
// 职责：对外暴露 MCP 工具，解析 JSON，调用内部接口。
// 这些函数由构造函数中的 lambda 回调调用。
// ================================================================


// ---------- 构造函数（内部注册所有 MCP 工具） ----------
CarController::CarController(
    gpio_num_t in1_a, gpio_num_t in2_a, gpio_num_t pwm_a,
    gpio_num_t in1_b, gpio_num_t in2_b, gpio_num_t pwm_b,
    gpio_num_t stby_pin,
    gpio_num_t enc_a, gpio_num_t enc_b,
    gpio_num_t enc_a2, gpio_num_t enc_b2)
    : in1_a_(in1_a), in2_a_(in2_a), pwm_a_(pwm_a),
        in1_b_(in1_b), in2_b_(in2_b), pwm_b_(pwm_b),
        stby_pin_(stby_pin),
        enc_a_pin_(enc_a), enc_b_pin_(enc_b),
        enc_a2_pin_(enc_a2), enc_b2_pin_(enc_b2)
{
    // ===== 1. 硬件初始化（LAYER 1） =====
    motor_gpio_init();
    motor_pwm_init();
    encoder_init();

    // ===== 2. PID 初始化（LAYER 2） =====
    pid_init(&pid_left_,  KP, KI, KD, DT_MS);
    pid_init(&pid_right_, KP, KI, KD, DT_MS);
    pid_left_.out_min = OUT_MIN;
    pid_left_.out_max = OUT_MAX;
    pid_right_.out_min = OUT_MIN;
    pid_right_.out_max = OUT_MAX;

    // 默认停止
    set_target_speed(0, 0);

    // ===== 3. 创建后台任务（LAYER 5） =====
    xTaskCreate(motor_control_task, "MotorPID", 4096, this, 5, &motor_ctrl_task_handle_);

    // ===== 4. 注册 MCP 工具 =====
    auto& server = McpServer::GetInstance();

    // ----- Tool 1: 设置速度 PID 参数 -----
    server.AddTool("car.set_speed_pid",
        "Set PID parameters for speed. All values are integers *100 (e.g., 800 = 8.00).",
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
            set_pid(lkp, lki, lkd, rkp, rki, rkd);
            return true;
        });

    // ----- Tool 2: 设置位置 PID 参数 -----
    server.AddTool("car.set_position_pid",
        "Set position PID Kp (integer *100, e.g., 50 = 0.50).",
        PropertyList({
            Property("position_kp", kPropertyTypeInteger, 50, 0, 10000),
        }),
        [this](const PropertyList& props) -> ReturnValue {
            float position_kp = props["position_kp"].value<int>() / 100.0f;
            set_position_pid(position_kp);
            return true;
        });

    // ----- Tool 3: 立即停止 -----
    server.AddTool("car.stop",
        "Stop the car immediately. This will cancel any ongoing movement.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            stop();
            return true;
        });

    // ----- Tool 4: 多段路径运动 -----
    server.AddTool("car.move_path",
        R"(Execute one or more continuous movement segments.
        Each segment: {"left": <speed>, "right": <speed>, "dur": <ms>}.
        Speeds: -6680 to 6680 (positive = forward, negative = backward).
        Duration in milliseconds. Segments are executed sequentially without stopping between them.

        Common motion examples (calibrated for default speed 3340):
        - Forward 1 sec:     [{"left":3340, "right":3340, "dur":1000}]
        - Backward 1 sec:    [{"left":-3340, "right":-3340, "dur":1000}]
        - Turn left 90°:     [{"left":-3340, "right":3340, "dur":250}]
        - Turn right 90°:    [{"left":3340, "right":-3340, "dur":250}]
        - U-turn (forward, turn 180°, forward): 
            [
                {"left":3340, "right":3340, "dur":1000},
                {"left":3340, "right":0, "dur":1500},
                {"left":3340, "right":4340, "dur":1000}
            ]
        - S-shape (left then right):
            [
                {"left":3340, "right":1340, "dur":800},
                {"left":1340, "right":3340, "dur":800},
            ]
        - Circle (continuous turn):
            [{"left":3340, "right":-3340, "dur":2000}]

        If speed not declared, use 3340.)",
        PropertyList({
            Property("segments", kPropertyTypeString)
        }),
        [this](const PropertyList& props) -> ReturnValue {
            std::string json = props["segments"].value<std::string>();
            return start_path(json);
        });

    ESP_LOGI("CarController", "MCP tools registered successfully.");
}

// ---------- 公共接口函数 ----------

// 立即停止
void CarController::stop() {
    force_stop_ = true;
    ESP_LOGI("CarController", "Stop command received");
}

// 设置速度 PID 参数
void CarController::set_pid(float kp_l, float ki_l, float kd_l,
                float kp_r, float ki_r, float kd_r) {
    pid_left_.Kp = kp_l;
    pid_left_.Ki = ki_l;
    pid_left_.Kd = kd_l;
    pid_right_.Kp = kp_r;
    pid_right_.Ki = ki_r;
    pid_right_.Kd = kd_r;
    pid_reset_both();
    ESP_LOGI("CarController", "PID updated: L(%.2f,%.2f,%.2f) R(%.2f,%.2f,%.2f)",
                kp_l, ki_l, kd_l, kp_r, ki_r, kd_r);
}

// 设置位置 PID 的 Kp
void CarController::set_position_pid(float kp) {
    position_kp_ = kp;
    ESP_LOGI("CarController", "Position Kp updated: %.2f", kp);
}

// 执行多段路径（JSON 数组）
bool CarController::start_path(const std::string& segments_json) {
    path_running_ = true;
    cJSON* root = cJSON_Parse(segments_json.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        ESP_LOGE("CarController", "Invalid segments JSON: %s", segments_json.c_str());
        path_running_=false;
        return false;
    }
    if (state_ == State::ACTIVE || state_ == State::RETURNING) {
        force_stop_ = true;
        int timeout = 0;
        while ((state_ != State::IDLE) && timeout < 50) {  // 最大等待 500ms
            vTaskDelay(pdMS_TO_TICKS(10));  // ✅ 让出 CPU
            timeout++;
        }
        force_stop_ = false;
    }
    state_ = State::ACTIVE;
    first_enter_ = true;

    int array_size = cJSON_GetArraySize(root);
    ESP_LOGI("CarController", "Starting path with %d segments", array_size);

    for (int i = 0; i < array_size; i++) {
        is_straight_=false;
        cJSON* seg = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(seg)) {
            ESP_LOGW("CarController", "Segment %d is not an object, skipping", i);
            continue;
        }

        cJSON* left = cJSON_GetObjectItem(seg, "left");
        cJSON* right = cJSON_GetObjectItem(seg, "right");
        cJSON* dur = cJSON_GetObjectItem(seg, "dur");

        if (!cJSON_IsNumber(left) || !cJSON_IsNumber(right) || !cJSON_IsNumber(dur)) {
            ESP_LOGW("CarController", "Segment %d missing valid left/right/dur, skipping", i);
            continue;
        }

        int l = left->valueint;
        int r = right->valueint;
        int d = dur->valueint;

        if(l==r) is_straight_=true;

        set_target_speed((float)l, (float)r);
        ESP_LOGI("CarController", "Segment %d: L=%d, R=%d, dur=%d ms", i, l, r, d);

        // 分段等待，便于响应 force_stop_
        const int steps = 5;
        int step_delay = d / steps;
        for (int j = 0; j < steps; j++) {
            if (force_stop_) {
                path_running_=false;
                set_target_speed(0, 0);
                ESP_LOGI("CarController", "Path interrupted by stop");
                cJSON_Delete(root);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(step_delay));
        }
    }
    is_straight_=false;
    path_running_ = false;
    set_target_speed(0, 0);
    cJSON_Delete(root);
    ESP_LOGI("CarController", "Path completed successfully");
    return true;
}

// 单段定时运动（兼容旧接口，内部复用 start_path）
// void move_timed(int left_speed, int right_speed, int duration_ms) {
//     char json_buf[128];
//     snprintf(json_buf, sizeof(json_buf),
//              "[{\"left\":%d,\"right\":%d,\"dur\":%d}]",
//              left_speed, right_speed, duration_ms);
//     start_path(std::string(json_buf));
// }

// ---------- 析构函数 ----------
CarController::~CarController() {
    if (motor_ctrl_task_handle_ != nullptr) {
        vTaskDelete(motor_ctrl_task_handle_);
    }
}

