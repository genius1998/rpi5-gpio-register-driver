/*
 * ssafy_gpio.c  -  GPIO 미들웨어 구현 (user space)
 *  /dev/ssafy_gpio 에 ioctl 을 보내는 wrapper + 버튼 디바운스 helper
 */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include "ssafy_gpio_ioctl.h"
#include "ssafy_gpio.h"

static int fd = -1;

static int map_errno(int e)
{
	switch (e) {
	case EINVAL: return SSAFY_ERR_VALUE;
	case EPERM:  return SSAFY_ERR_MODE;
	case EIO:    return SSAFY_ERR_FAULT;
	case EBUSY:  return SSAFY_ERR_BUSY;
	default:     return SSAFY_ERR_IO;
	}
}

static int check(int pin)
{
	if (fd < 0)
		return SSAFY_ERR_NOT_OPEN;
	if (pin < SSAFY_PIN_MIN || pin > SSAFY_PIN_MAX)
		return SSAFY_ERR_PIN;
	return SSAFY_OK;
}

static int req(unsigned long cmd, int pin, int value, int *out)
{
	struct ssafy_gpio_req r = { .pin = (unsigned)pin, .value = value };
	int ret = check(pin);

	if (ret)
		return ret;
	if (ioctl(fd, cmd, &r) < 0)
		return map_errno(errno);
	if (out)
		*out = r.value;
	return SSAFY_OK;
}

int ssafy_gpio_open(void)
{
	if (fd >= 0)
		return SSAFY_OK;
	fd = open("/dev/" SSAFY_GPIO_DEV_NAME, O_RDWR);
	if (fd < 0) {
		perror("open /dev/" SSAFY_GPIO_DEV_NAME);
		return SSAFY_ERR_NOT_OPEN;
	}
	return SSAFY_OK;
}

void ssafy_gpio_close(void)
{
	if (fd >= 0)
		close(fd);          /* 드라이버가 release() 에서 출력 OFF 처리 */
	fd = -1;
}

int ssafy_gpio_pinMode(int pin, int mode)
{
	if (mode != INPUT && mode != OUTPUT)
		return SSAFY_ERR_VALUE;
	return req(SSAFY_IOC_PINMODE, pin, mode, NULL);
}

int ssafy_gpio_output(int pin, int value)
{
	if (value != LOW && value != HIGH)
		return SSAFY_ERR_VALUE;
	return req(SSAFY_IOC_WRITE, pin, value, NULL);
}

int ssafy_gpio_input(int pin)
{
	int v = 0;
	int ret = req(SSAFY_IOC_READ, pin, 0, &v);

	return ret ? ret : v;
}

int ssafy_gpio_pwm(int pin, int duty)
{
	if (duty < 0 || duty > 100)
		return SSAFY_ERR_VALUE;
	return req(SSAFY_IOC_PWM, pin, duty, NULL);
}

int ssafy_gpio_diag(int pin, ssafy_diag_t *d)
{
	struct ssafy_gpio_diag k = { .pin = (unsigned)pin };
	int ret = check(pin);

	if (ret)
		return ret;
	if (!d)
		return SSAFY_ERR_VALUE;
	if (ioctl(fd, SSAFY_IOC_DIAG, &k) < 0)
		return map_errno(errno);
	d->mode = k.mode;
	d->commanded = k.commanded;
	d->actual = k.actual;
	d->pwm_duty = k.pwm_duty;
	d->fault = k.fault;
	d->mismatch_cnt = k.mismatch_cnt;
	return SSAFY_OK;
}

int ssafy_gpio_clearFault(int pin)
{
	return req(SSAFY_IOC_CLEAR_FAULT, pin, 0, NULL);
}

const char *ssafy_gpio_strerror(int err)
{
	switch (err) {
	case SSAFY_OK:           return "OK";
	case SSAFY_ERR_NOT_OPEN: return "device not open";
	case SSAFY_ERR_PIN:      return "invalid pin (GPIO2~27 only)";
	case SSAFY_ERR_MODE:     return "wrong pin mode";
	case SSAFY_ERR_VALUE:    return "invalid value";
	case SSAFY_ERR_FAULT:    return "output readback fault (output forced OFF)";
	case SSAFY_ERR_BUSY:     return "PWM channel busy";
	default:                 return "I/O error";
	}
}

void ssafy_gpio_help(void)
{
	printf(
	"ssafy_gpio API\n"
	"  int ssafy_gpio_open(void)                  /dev/ssafy_gpio 열기\n"
	"  void ssafy_gpio_close(void)                닫기 (모든 출력 OFF)\n"
	"  int ssafy_gpio_pinMode(pin, INPUT|OUTPUT)  핀 모드 설정\n"
	"  int ssafy_gpio_output(pin, LOW|HIGH)       출력 + readback 검사\n"
	"  int ssafy_gpio_input(pin)                  핀 레벨 0/1 읽기\n"
	"  int ssafy_gpio_pwm(pin, duty 0~100)        소프트웨어 PWM(100Hz)\n"
	"  int ssafy_gpio_diag(pin, &diag)            명령값/실제값/fault 조회\n"
	"  int ssafy_gpio_clearFault(pin)             fault latch 해제\n"
	"  int ssafy_button_init(&b, pin, active_low, debounce_ms)\n"
	"  int ssafy_button_update(&b, now_ms)        디바운스된 눌림 0/1\n"
	"return: 0 이상 성공 / 음수 에러\n"
	"  -1 NOT_OPEN  -2 PIN  -3 MODE  -4 VALUE  -5 FAULT  -6 BUSY  -7 IO\n");
}

long ssafy_millis(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int ssafy_button_init(ssafy_button_t *b, int pin, int active_low, int debounce_ms)
{
	int ret, v;

	if (!b || debounce_ms < 0)
		return SSAFY_ERR_VALUE;
	ret = ssafy_gpio_pinMode(pin, INPUT);
	if (ret)
		return ret;
	v = ssafy_gpio_input(pin);
	if (v < 0)
		return v;
	b->pin = pin;
	b->active_low = active_low;
	b->debounce_ms = debounce_ms;
	b->raw = active_low ? !v : v;
	b->raw_since_ms = ssafy_millis();
	b->pressed = b->raw;
	b->pressed_since_ms = b->raw_since_ms;
	b->raw_edges = 0;
	b->clean_edges = 0;
	return SSAFY_OK;
}

int ssafy_button_update(ssafy_button_t *b, long now_ms)
{
	int v = ssafy_gpio_input(b->pin);
	int raw;

	if (v < 0)
		return v;
	raw = b->active_low ? !v : v;
	if (raw != b->raw) {                       /* 바뀌었으면 시간 재기 시작 */
		b->raw = raw;
		b->raw_since_ms = now_ms;
		b->raw_edges++;
	}
	/* debounce_ms 동안 같은 값이 유지되어야 상태로 인정 */
	if (raw != b->pressed && now_ms - b->raw_since_ms >= b->debounce_ms) {
		b->pressed = raw;
		b->pressed_since_ms = now_ms;
		b->clean_edges++;
	}
	return b->pressed;
}
