/*
 * api_test.c  -  ssafy_gpio API 자동 점검 (Day3 회로 연결 상태에서 실행)
 *   ./api_test          : 기본 점검
 *   sudo ./api_test     : + 고장 주입(readback fault) 점검까지
 */
#include <stdio.h>
#include <unistd.h>
#include "ssafy_gpio.h"

#define LED 20
#define BTN 2
#define FAULT_PARAM "/sys/module/ssafy_gpio_drv/parameters/inject_fault"

static int pass, fail;

static void expect(const char *name, int got, int want)
{
	if (got == want) {
		pass++;
		printf("[PASS] %-42s -> %d\n", name, got);
	} else {
		fail++;
		printf("[FAIL] %-42s -> %d (expected %d: %s)\n",
		       name, got, want, ssafy_gpio_strerror(want));
	}
}

static int set_inject(int on)
{
	FILE *f = fopen(FAULT_PARAM, "w");

	if (!f)
		return -1;
	fprintf(f, "%d\n", on);
	fclose(f);
	return 0;
}

int main(void)
{
	ssafy_diag_t d;

	expect("output before open", ssafy_gpio_output(LED, HIGH), SSAFY_ERR_NOT_OPEN);
	if (ssafy_gpio_open() != SSAFY_OK)
		return 1;

	/* 1. 입력 검증 */
	expect("pinMode(1)  out of range", ssafy_gpio_pinMode(1, OUTPUT), SSAFY_ERR_PIN);
	expect("pinMode(28) out of range", ssafy_gpio_pinMode(28, OUTPUT), SSAFY_ERR_PIN);
	expect("pinMode(20, 5) bad mode", ssafy_gpio_pinMode(LED, 5), SSAFY_ERR_VALUE);

	/* 2. 정상 설정 */
	expect("pinMode(20, OUTPUT)", ssafy_gpio_pinMode(LED, OUTPUT), SSAFY_OK);
	expect("pinMode(2, INPUT)", ssafy_gpio_pinMode(BTN, INPUT), SSAFY_OK);

	/* 3. 모드 위반 */
	expect("output() on INPUT pin 2", ssafy_gpio_output(BTN, HIGH), SSAFY_ERR_MODE);
	expect("output(20, 7) bad level", ssafy_gpio_output(LED, 7), SSAFY_ERR_VALUE);
	expect("pwm(20, 150) bad duty", ssafy_gpio_pwm(LED, 150), SSAFY_ERR_VALUE);

	/* 4. 출력 + readback */
	expect("output(20, HIGH)", ssafy_gpio_output(LED, HIGH), SSAFY_OK);
	expect("readback input(20) == 1", ssafy_gpio_input(LED), 1);
	usleep(300 * 1000);
	expect("output(20, LOW)", ssafy_gpio_output(LED, LOW), SSAFY_OK);
	expect("readback input(20) == 0", ssafy_gpio_input(LED), 0);

	/* 5. PWM */
	expect("pwm(20, 50)", ssafy_gpio_pwm(LED, 50), SSAFY_OK);
	ssafy_gpio_diag(LED, &d);
	expect("diag pwm_duty == 50", d.pwm_duty, 50);
	usleep(500 * 1000);
	ssafy_gpio_diag(LED, &d);
	expect("diag no fault during PWM", d.fault, 0);
	expect("pwm(20, 0) -> OFF", ssafy_gpio_pwm(LED, 0), SSAFY_OK);

	/* 6. 버튼 (풀업, 안 누른 상태 = 1) */
	printf("       (버튼에서 손 떼고 실행)\n");
	expect("input(2) released == 1", ssafy_gpio_input(BTN), 1);

	/* 7. 고장 주입 */
	if (set_inject(1) == 0) {
		expect("[inject] output(20, HIGH) -> FAULT", ssafy_gpio_output(LED, HIGH), SSAFY_ERR_FAULT);
		set_inject(0);
		ssafy_gpio_diag(LED, &d);
		expect("[inject] LED forced OFF (actual 0)", d.actual, 0);
		expect("[inject] fault latched", ssafy_gpio_output(LED, HIGH), SSAFY_ERR_FAULT);
		expect("[inject] clearFault", ssafy_gpio_clearFault(LED), SSAFY_OK);
		expect("[inject] output(20, HIGH) recovers", ssafy_gpio_output(LED, HIGH), SSAFY_OK);
		ssafy_gpio_output(LED, LOW);
	} else {
		printf("[SKIP] fault injection (sudo 로 실행하면 점검)\n");
	}

	ssafy_gpio_close();
	printf("\nresult: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
