/*
 * switch_app.c  -  버튼 LED 밝기 스위치 모듈 (ssafy_gpio API 만 사용)
 *
 *  짧게 누름(<0.7s)        : LED ON/OFF 토글
 *  길게 누름(>=0.7s)       : 0.4s 마다 밝기 20→40→60→80→100→20 % 순환
 *  계속 누름(>=5s)         : BUTTON_STUCK 고장 → 입력 무시 + 보고, 떼면 해제
 *  출력 readback 불일치    : LED_OUTPUT_FAULT → 드라이버가 LED 강제 OFF,
 *                            짧게 누르면 fault 해제 후 재시도
 *  Ctrl+C / 비정상 종료    : LED OFF (드라이버 release() fail-safe)
 *
 *  회로 (Day3 PDF)
 *   LED(+) → GPIO20, LED(-) → 220Ω → GND
 *   버튼 → GPIO2, 버튼 → 10kΩ → 3.3V(풀업), 버튼 → GND   (누르면 0)
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "ssafy_gpio.h"

#define LED_PIN       20
#define BTN_PIN       2
#define DEBOUNCE_MS   30
#define LONG_MS       700
#define STEP_MS       400
#define STUCK_MS      5000
#define LOOP_MS       5
#define DIAG_MS       100

static const int levels[] = { 20, 40, 60, 80, 100 };
#define NLEVEL ((int)(sizeof(levels) / sizeof(levels[0])))

static volatile sig_atomic_t running = 1;
static long t0;

static void on_sigint(int sig) { (void)sig; running = 0; }

static void logf_(const char *tag, const char *msg)
{
	long t = ssafy_millis() - t0;

	printf("[%4ld.%03lds] %-6s %s\n", t / 1000, t % 1000, tag, msg);
	fflush(stdout);
}

static int  led_on;
static int  idx = 2;          /* 시작 밝기 60% */
static int  out_fault;

static void show_led(const char *why)
{
	char buf[96];

	if (led_on)
		snprintf(buf, sizeof(buf), "LED ON  %3d%%  (%s)", levels[idx], why);
	else
		snprintf(buf, sizeof(buf), "LED OFF       (%s)", why);
	logf_("[LED]", buf);
}

/* 현재 상태를 LED 에 반영. 실패하면 고장 처리 */
static void apply(const char *why)
{
	int ret = ssafy_gpio_pwm(LED_PIN, led_on ? levels[idx] : 0);

	if (ret == SSAFY_OK) {
		show_led(why);
		return;
	}
	if (ret == SSAFY_ERR_FAULT) {
		if (!out_fault)
			logf_("[DTC]", "LED_OUTPUT_FAULT: readback mismatch -> LED forced OFF");
		out_fault = 1;
		return;
	}
	logf_("[ERR]", ssafy_gpio_strerror(ret));
}

int main(int argc, char **argv)
{
	ssafy_button_t btn;
	ssafy_diag_t d;
	int prev = 0, stuck = 0, long_active = 0, ret;
	long press_start = 0, last_step = 0, last_diag = 0;
	char buf[128];

	if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h"))) {
		ssafy_gpio_help();
		return 0;
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);
	t0 = ssafy_millis();

	if (ssafy_gpio_open() != SSAFY_OK)
		return 1;
	if ((ret = ssafy_gpio_pinMode(LED_PIN, OUTPUT)) != SSAFY_OK ||
	    (ret = ssafy_button_init(&btn, BTN_PIN, 1, DEBOUNCE_MS)) != SSAFY_OK) {
		fprintf(stderr, "init failed: %s\n", ssafy_gpio_strerror(ret));
		ssafy_gpio_close();
		return 1;
	}
	ssafy_gpio_clearFault(LED_PIN);
	logf_("[SYS]", "switch module start (short: toggle / long: brightness / Ctrl+C: exit)");
	apply("init");

	/* 시작부터 눌려 있으면 stuck 후보: 떼기 전까지 동작하지 않음 */
	prev = btn.pressed;
	press_start = ssafy_millis();
	if (prev)
		logf_("[WARN]", "button pressed at startup, waiting for release");

	while (running) {
		long now = ssafy_millis();
		int p = ssafy_button_update(&btn, now);

		if (p < 0) {
			logf_("[ERR]", ssafy_gpio_strerror(p));
			break;
		}

		/* 눌림 시작 */
		if (p && !prev) {
			press_start = now;
			last_step = now;
			long_active = 0;
			logf_("[BTN]", "pressed");
		}

		/* 누르고 있는 동안 */
		if (p && !stuck) {
			long held = now - press_start;

			if (held >= STUCK_MS) {
				stuck = 1;
				snprintf(buf, sizeof(buf),
					 "BUTTON_STUCK: held > %d ms -> input ignored until release",
					 STUCK_MS);
				logf_("[DTC]", buf);
			} else if (!out_fault && held >= LONG_MS &&
				   (!long_active || now - last_step >= STEP_MS)) {
				if (!led_on)
					led_on = 1;            /* 꺼져 있으면 켜기부터 */
				else
					idx = (idx + 1) % NLEVEL;
				long_active = 1;
				last_step = now;
				apply("long press");
			}
		}

		/* 뗌 */
		if (!p && prev) {
			long held = now - press_start;

			snprintf(buf, sizeof(buf), "released (%ld ms)", held);
			logf_("[BTN]", buf);
			if (stuck) {
				stuck = 0;
				logf_("[DTC]", "BUTTON_STUCK cleared (button released)");
			} else if (out_fault) {
				logf_("[SYS]", "retry: clear LED fault");
				ssafy_gpio_clearFault(LED_PIN);
				out_fault = 0;
				apply("fault retry");
			} else if (!long_active) {
				led_on = !led_on;
				apply("short press");
			}
		}
		prev = p;

		/* 주기 진단: PWM 중 readback 고장은 드라이버가 감지해 latch 함 */
		if (now - last_diag >= DIAG_MS) {
			last_diag = now;
			if (ssafy_gpio_diag(LED_PIN, &d) == SSAFY_OK && d.fault && !out_fault) {
				out_fault = 1;
				snprintf(buf, sizeof(buf),
					 "LED_OUTPUT_FAULT: mismatch %d times -> LED forced OFF",
					 d.mismatch_cnt);
				logf_("[DTC]", buf);
			}
		}
		usleep(LOOP_MS * 1000);
	}

	ssafy_gpio_pwm(LED_PIN, 0);
	snprintf(buf, sizeof(buf), "exit, LED OFF (button edges raw %d / debounced %d)",
		 btn.raw_edges, btn.clean_edges);
	logf_("[SYS]", buf);
	ssafy_gpio_close();
	return 0;
}
