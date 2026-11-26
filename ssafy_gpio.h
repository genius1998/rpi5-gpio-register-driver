/*
 * ssafy_gpio.h  -  GPIO 미들웨어 API (user space)
 *  app 개발자는 비트 연산/ioctl 없이 이 함수들만 사용한다. ([도전 4] 확장)
 *
 *  모든 함수는 성공 시 0 이상, 실패 시 SSAFY_ERR_* (음수) 를 반환한다.
 */
#ifndef SSAFY_GPIO_H
#define SSAFY_GPIO_H

#define INPUT   0
#define OUTPUT  1
#define LOW     0
#define HIGH    1

/* 에러 코드 */
#define SSAFY_OK            0
#define SSAFY_ERR_NOT_OPEN  (-1)   /* ssafy_gpio_open() 안 함 / 장치 파일 없음 */
#define SSAFY_ERR_PIN       (-2)   /* 허용 범위(GPIO2~27) 밖의 핀 */
#define SSAFY_ERR_MODE      (-3)   /* 모드가 맞지 않음 (예: INPUT 핀에 output) */
#define SSAFY_ERR_VALUE     (-4)   /* 값 범위 오류 (레벨 0/1, duty 0~100) */
#define SSAFY_ERR_FAULT     (-5)   /* 출력 readback 고장 → 출력 OFF 상태로 잠김 */
#define SSAFY_ERR_BUSY      (-6)   /* PWM 채널 사용 중 */
#define SSAFY_ERR_IO        (-7)   /* 그 밖의 시스템 오류 */

typedef struct {
	int mode;          /* INPUT / OUTPUT */
	int commanded;     /* 마지막 명령 레벨, PWM 중이면 -1 */
	int actual;        /* 실제 핀 레벨 */
	int pwm_duty;      /* PWM duty(%) */
	int fault;         /* 0 이면 정상 */
	int mismatch_cnt;  /* readback 불일치 누적 */
} ssafy_diag_t;

int  ssafy_gpio_open(void);
void ssafy_gpio_close(void);

int  ssafy_gpio_pinMode(int pin, int mode);
int  ssafy_gpio_output(int pin, int value);
int  ssafy_gpio_input(int pin);                 /* 0/1 또는 에러 */
int  ssafy_gpio_pwm(int pin, int duty);         /* duty 0~100 */
int  ssafy_gpio_diag(int pin, ssafy_diag_t *d);
int  ssafy_gpio_clearFault(int pin);

const char *ssafy_gpio_strerror(int err);
void ssafy_gpio_help(void);

/* ---- 버튼 helper : 디바운스 ---- */
typedef struct {
	int  pin;
	int  active_low;     /* 풀업 회로면 1 (누르면 0) */
	int  debounce_ms;
	int  raw;            /* 마지막 raw 값 */
	long raw_since_ms;   /* raw 가 이 값으로 바뀐 시각 */
	int  pressed;        /* 디바운스된 상태 (1 = 눌림) */
	long pressed_since_ms;
	int  raw_edges;       /* raw 변화 횟수 (채터링 포함) */
	int  clean_edges;     /* 디바운스 후 변화 횟수 */
} ssafy_button_t;

int ssafy_button_init(ssafy_button_t *b, int pin, int active_low, int debounce_ms);
/* now_ms 시점의 디바운스된 눌림 상태(0/1) 또는 에러 반환 */
int ssafy_button_update(ssafy_button_t *b, long now_ms);

long ssafy_millis(void);

#endif
