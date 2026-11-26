/*
 * ssafy_gpio_ioctl.h
 *  - 커널 드라이버(ssafy_gpio_drv.c)와 user space 미들웨어(ssafy_gpio.c)가
 *    "같이" include 하는 ioctl 인터페이스 정의
 *  - Day2/3 실습의 _IO(0,num) 대신 방향(_IOW/_IOR/_IOWR)과 magic 을 명시
 */
#ifndef SSAFY_GPIO_IOCTL_H
#define SSAFY_GPIO_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SSAFY_GPIO_DEV_NAME   "ssafy_gpio"     /* /dev/ssafy_gpio */
#define SSAFY_GPIO_MAGIC      'G'

#define SSAFY_PIN_MIN         2                /* 헤더 핀 GPIO2 ~ GPIO27 만 허용 */
#define SSAFY_PIN_MAX         27

#define SSAFY_MODE_INPUT      0
#define SSAFY_MODE_OUTPUT     1

/* diag.fault 비트 */
#define SSAFY_FAULT_READBACK  (1u << 0)        /* 출력 명령값 != 실제 핀 레벨 */

struct ssafy_gpio_req {
	__u32 pin;
	__s32 value;   /* mode / level / duty(0~100) / 읽은 값 */
};

struct ssafy_gpio_diag {
	__u32 pin;
	__u32 mode;          /* SSAFY_MODE_* */
	__s32 commanded;     /* 마지막으로 명령한 출력 레벨 (PWM 중이면 -1) */
	__s32 actual;        /* 지금 핀에서 읽은 실제 레벨 */
	__u32 pwm_duty;      /* 0~100, PWM 미사용이면 0 */
	__u32 fault;         /* SSAFY_FAULT_* 비트 (latch) */
	__u32 mismatch_cnt;  /* readback 불일치 누적 횟수 */
};

#define SSAFY_IOC_PINMODE      _IOW (SSAFY_GPIO_MAGIC, 1, struct ssafy_gpio_req)
#define SSAFY_IOC_WRITE        _IOW (SSAFY_GPIO_MAGIC, 2, struct ssafy_gpio_req)
#define SSAFY_IOC_READ         _IOWR(SSAFY_GPIO_MAGIC, 3, struct ssafy_gpio_req)
#define SSAFY_IOC_PWM          _IOW (SSAFY_GPIO_MAGIC, 4, struct ssafy_gpio_req)
#define SSAFY_IOC_DIAG         _IOWR(SSAFY_GPIO_MAGIC, 5, struct ssafy_gpio_diag)
#define SSAFY_IOC_CLEAR_FAULT  _IOW (SSAFY_GPIO_MAGIC, 6, struct ssafy_gpio_req)

#endif
