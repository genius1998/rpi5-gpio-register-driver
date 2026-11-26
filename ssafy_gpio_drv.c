// SPDX-License-Identifier: GPL-2.0
/*
 * ssafy_gpio_drv.c  -  RP1(Raspberry Pi 5) GPIO 레지스터 직접 제어 캐릭터 드라이버
 *
 * Day3 LED/버튼 실습 코드(ioremap + GPIOn_CTRL/STATUS + PADS_BANK0)를
 * "범용 GPIO 서비스"로 확장한 버전.
 *
 *  - ioctl API : PINMODE / WRITE / READ / PWM / DIAG / CLEAR_FAULT
 *  - 핀 검증    : GPIO2~27 만 허용, 모드가 맞지 않는 접근은 에러 코드 반환
 *  - PWM        : hrtimer 기반 소프트웨어 PWM (100Hz, duty 0~100%)
 *  - 진단       : 출력 readback (명령한 레벨 vs 실제 핀 레벨) → 불일치 시 fault latch
 *  - fail-safe  : fault 발생 / app 종료(close) / rmmod 시 모든 출력 LOW
 *
 * 레지스터 근거 (Day3 PDF, RP1 datasheet)
 *  - 0x1f_0000_0000 = RP1 peripheral base (32bit 기준 0x4000_0000)
 *  - io_bank0  = +0xd0000 : GPIOn_STATUS = n*8, GPIOn_CTRL = n*8 + 4
 *                           (GPIO2_STATUS 0x010, GPIO2_CTRL 0x014, GPIO20_CTRL 0x0a4)
 *  - pads_bank0= +0xf0000 : GPIOn pad    = 0x04 + n*4  (GPIO2 0x0c, GPIO20 0x54)
 *  - CTRL  4:0 FUNCSEL(0x7), 13:12 OUTOVER(0x2 LOW / 0x3 HIGH),
 *          15:14 OEOVER(0x2 disable / 0x3 enable), 17:16 INOVER(0x0)
 *  - STATUS 23 EVENT_LEVEL_HIGH (핀 입력 레벨)
 *  - PAD   7 OD(output disable), 6 IE(input enable), 5:4 DRIVE(0x1 = 4mA)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include "ssafy_gpio_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSAFY RP1 GPIO driver with PWM and readback diagnostics");

/* ---------- 레지스터 정의 ---------- */
#define RP1_PERI_BASE      0x1f00000000ULL
#define RP1_MAP_SIZE       (64 * 1024 * 1024)
#define IO_BANK0_OFS       0xd0000
#define PADS_BANK0_OFS     0xf0000

#define GPIO_STATUS(n)     (io_bank0 + (n) * 8)
#define GPIO_CTRL(n)       (io_bank0 + (n) * 8 + 4)
#define PAD_GPIO(n)        (pads_bank0 + 0x04 + (n) * 4)

#define CTRL_FUNCSEL_MASK  (0x1f << 0)
#define CTRL_FUNCSEL_PIO   (0x7 << 0)
#define CTRL_OUTOVER_MASK  (0x3 << 12)
#define CTRL_OUTOVER_LOW   (0x2 << 12)
#define CTRL_OUTOVER_HIGH  (0x3 << 12)
#define CTRL_OEOVER_MASK   (0x3 << 14)
#define CTRL_OEOVER_DIS    (0x2 << 14)
#define CTRL_OEOVER_EN     (0x3 << 14)
#define CTRL_INOVER_MASK   (0x3 << 16)

#define STATUS_LEVEL_HIGH  23

#define PAD_OD             (1 << 7)
#define PAD_IE             (1 << 6)
#define PAD_DRIVE_MASK     (0x3 << 4)
#define PAD_DRIVE_4MA      (0x1 << 4)

/* ---------- PWM / 진단 파라미터 ---------- */
#define PWM_PERIOD_US      10000   /* 10ms = 100Hz (눈에 깜빡임이 안 보이는 주기) */
#define READBACK_DELAY_US  2       /* 출력 후 pad 레벨이 안정될 때까지 대기 */
#define PWM_FAULT_STREAK   3       /* PWM 중 연속 N번 불일치하면 fault */

/* 고장 주입(시연/테스트용): 1 이면 출력핀 readback 값을 반전시켜 단락 상황을 흉내냄 */
static bool inject_fault;
module_param(inject_fault, bool, 0644);
MODULE_PARM_DESC(inject_fault, "simulate output readback mismatch (test only)");

struct pin_state {
	bool used;
	u8   mode;        /* SSAFY_MODE_INPUT / OUTPUT */
	s8   commanded;   /* 0/1, PWM 중이면 -1 */
	u32  fault;
	u32  mismatch;
};

static void __iomem *peri_base;
static void __iomem *io_bank0;
static void __iomem *pads_bank0;

static struct pin_state pins[SSAFY_PIN_MAX + 1];
static DEFINE_SPINLOCK(gpio_lock);

static struct hrtimer pwm_timer;
static int  pwm_pin = -1;
static u32  pwm_duty;
static int  pwm_level;
static int  pwm_streak;

static int NOD_MAJOR;
static struct class *cls;
static dev_t dev;
static atomic_t open_cnt = ATOMIC_INIT(0);

/* ---------- low level (gpio_lock 잡은 상태에서 호출) ---------- */
static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	u32 v = readl(reg);

	v &= ~mask;
	v |= val;
	writel(v, reg);
}

static void drive(int pin, int level)
{
	rmw(GPIO_CTRL(pin), CTRL_OUTOVER_MASK,
	    level ? CTRL_OUTOVER_HIGH : CTRL_OUTOVER_LOW);
	(void)readl(GPIO_CTRL(pin));          /* posted write flush */
}

static int pad_level(int pin)
{
	int lv = (readl(GPIO_STATUS(pin)) >> STATUS_LEVEL_HIGH) & 0x1;

	if (inject_fault && pins[pin].mode == SSAFY_MODE_OUTPUT)
		lv = !lv;
	return lv;
}

static bool valid_pin(u32 pin)
{
	return pin >= SSAFY_PIN_MIN && pin <= SSAFY_PIN_MAX;
}

/* ---------- PWM ---------- */
static enum hrtimer_restart pwm_cb(struct hrtimer *t)
{
	unsigned long flags;
	u32 next_us;
	int pin, actual;

	spin_lock_irqsave(&gpio_lock, flags);
	pin = pwm_pin;
	if (pin < 0) {
		spin_unlock_irqrestore(&gpio_lock, flags);
		return HRTIMER_NORESTART;
	}

	/* 직전 edge 에서 명령한 레벨이 실제로 핀에 나와 있는지 확인 */
	actual = pad_level(pin);
	if (actual != pwm_level) {
		pins[pin].mismatch++;
		if (++pwm_streak >= PWM_FAULT_STREAK) {
			drive(pin, 0);                  /* fail-safe OFF */
			pins[pin].fault |= SSAFY_FAULT_READBACK;
			pins[pin].commanded = 0;
			pwm_pin = -1;
			spin_unlock_irqrestore(&gpio_lock, flags);
			pr_err("ssafy_gpio: GPIO%d PWM readback fault -> output OFF\n", pin);
			return HRTIMER_NORESTART;
		}
	} else {
		pwm_streak = 0;
	}

	pwm_level = !pwm_level;
	drive(pin, pwm_level);
	next_us = pwm_level ? PWM_PERIOD_US * pwm_duty / 100
			    : PWM_PERIOD_US * (100 - pwm_duty) / 100;
	spin_unlock_irqrestore(&gpio_lock, flags);

	hrtimer_forward_now(t, ns_to_ktime((u64)(next_us) * NSEC_PER_USEC));
	return HRTIMER_RESTART;
}

/* PWM 정지 (gpio_lock 잡지 않은 상태에서 호출: hrtimer_cancel 은 콜백 종료를 기다림) */
static void pwm_stop(void)
{
	unsigned long flags;
	int p;

	spin_lock_irqsave(&gpio_lock, flags);
	p = pwm_pin;
	pwm_pin = -1;
	spin_unlock_irqrestore(&gpio_lock, flags);

	hrtimer_cancel(&pwm_timer);

	if (p >= 0) {
		spin_lock_irqsave(&gpio_lock, flags);
		drive(p, 0);
		pins[p].commanded = 0;
		spin_unlock_irqrestore(&gpio_lock, flags);
	}
}

static void failsafe_all_off(const char *why)
{
	unsigned long flags;
	int i;

	pwm_stop();
	spin_lock_irqsave(&gpio_lock, flags);
	for (i = SSAFY_PIN_MIN; i <= SSAFY_PIN_MAX; i++) {
		if (pins[i].used && pins[i].mode == SSAFY_MODE_OUTPUT) {
			drive(i, 0);
			pins[i].commanded = 0;
		}
	}
	spin_unlock_irqrestore(&gpio_lock, flags);
	pr_info("ssafy_gpio: fail-safe, all outputs LOW (%s)\n", why);
}

/* ---------- API 구현 ---------- */
static int do_pinmode(u32 pin, s32 mode)
{
	unsigned long flags;

	if (!valid_pin(pin))
		return -EINVAL;
	if (mode != SSAFY_MODE_INPUT && mode != SSAFY_MODE_OUTPUT)
		return -EINVAL;
	if (pwm_pin == (int)pin)
		pwm_stop();

	spin_lock_irqsave(&gpio_lock, flags);
	rmw(GPIO_CTRL(pin), CTRL_FUNCSEL_MASK, CTRL_FUNCSEL_PIO);
	rmw(GPIO_CTRL(pin), CTRL_INOVER_MASK, 0);
	if (mode == SSAFY_MODE_OUTPUT) {
		rmw(GPIO_CTRL(pin), CTRL_OUTOVER_MASK, CTRL_OUTOVER_LOW); /* 먼저 LOW */
		rmw(GPIO_CTRL(pin), CTRL_OEOVER_MASK, CTRL_OEOVER_EN);
		/* OD 해제, readback 을 위해 IE 켬, 4mA */
		rmw(PAD_GPIO(pin), PAD_OD | PAD_IE | PAD_DRIVE_MASK,
		    PAD_IE | PAD_DRIVE_4MA);
	} else {
		rmw(GPIO_CTRL(pin), CTRL_OEOVER_MASK, CTRL_OEOVER_DIS);
		rmw(PAD_GPIO(pin), PAD_IE, PAD_IE);
	}
	pins[pin].used = true;
	pins[pin].mode = mode;
	pins[pin].commanded = 0;
	spin_unlock_irqrestore(&gpio_lock, flags);

	pr_info("ssafy_gpio: GPIO%u -> %s\n", pin,
		mode == SSAFY_MODE_OUTPUT ? "OUTPUT" : "INPUT");
	return 0;
}

static int do_write(u32 pin, s32 value)
{
	unsigned long flags;
	int actual;

	if (!valid_pin(pin) || (value != 0 && value != 1))
		return -EINVAL;
	if (!pins[pin].used || pins[pin].mode != SSAFY_MODE_OUTPUT)
		return -EPERM;
	if (pins[pin].fault)
		return -EIO;
	if (pwm_pin == (int)pin)
		pwm_stop();

	spin_lock_irqsave(&gpio_lock, flags);
	drive(pin, value);
	udelay(READBACK_DELAY_US);
	actual = pad_level(pin);
	if (actual != value) {
		pins[pin].mismatch++;
		pins[pin].fault |= SSAFY_FAULT_READBACK;
		drive(pin, 0);                          /* fail-safe OFF */
		pins[pin].commanded = 0;
		spin_unlock_irqrestore(&gpio_lock, flags);
		pr_err("ssafy_gpio: GPIO%u readback fault (cmd %d, pin %d) -> OFF\n",
		       pin, value, actual);
		return -EIO;
	}
	pins[pin].commanded = value;
	spin_unlock_irqrestore(&gpio_lock, flags);
	return 0;
}

static int do_read(u32 pin, s32 *value)
{
	unsigned long flags;

	if (!valid_pin(pin))
		return -EINVAL;
	if (!pins[pin].used)
		return -EPERM;
	spin_lock_irqsave(&gpio_lock, flags);
	*value = pad_level(pin);
	spin_unlock_irqrestore(&gpio_lock, flags);
	return 0;
}

static int do_pwm(u32 pin, s32 duty)
{
	unsigned long flags;
	bool start = false;

	if (!valid_pin(pin) || duty < 0 || duty > 100)
		return -EINVAL;
	if (!pins[pin].used || pins[pin].mode != SSAFY_MODE_OUTPUT)
		return -EPERM;
	if (pins[pin].fault)
		return -EIO;
	if (pwm_pin >= 0 && pwm_pin != (int)pin)
		return -EBUSY;                          /* PWM 채널은 1개 */

	/* 0%, 100% 는 PWM 없이 정적 출력 (+ readback 검사) */
	if (duty == 0 || duty == 100)
		return do_write(pin, duty == 100);

	spin_lock_irqsave(&gpio_lock, flags);
	pwm_duty = duty;
	if (pwm_pin < 0) {
		pwm_pin = pin;
		pwm_level = 1;
		pwm_streak = 0;
		pins[pin].commanded = -1;
		drive(pin, 1);
		start = true;
	}
	spin_unlock_irqrestore(&gpio_lock, flags);

	if (start)
		hrtimer_start(&pwm_timer,
			      ns_to_ktime((u64)PWM_PERIOD_US * duty / 100 * NSEC_PER_USEC),
			      HRTIMER_MODE_REL);
	return 0;
}

static int do_diag(struct ssafy_gpio_diag *d)
{
	unsigned long flags;
	u32 pin = d->pin;

	if (!valid_pin(pin))
		return -EINVAL;
	if (!pins[pin].used)
		return -EPERM;
	spin_lock_irqsave(&gpio_lock, flags);
	d->mode = pins[pin].mode;
	d->commanded = pins[pin].commanded;
	d->actual = pad_level(pin);
	/* 정적 출력(ON 100% / OFF)도 주기적으로 readback 감시 */
	if (pins[pin].mode == SSAFY_MODE_OUTPUT && pins[pin].commanded >= 0 &&
	    !pins[pin].fault && d->actual != pins[pin].commanded) {
		pins[pin].mismatch++;
		pins[pin].fault |= SSAFY_FAULT_READBACK;
		drive(pin, 0);                          /* fail-safe OFF */
		pins[pin].commanded = 0;
		pr_err("ssafy_gpio: GPIO%u readback fault in diag -> OFF\n", pin);
	}
	d->pwm_duty = (pwm_pin == (int)pin) ? pwm_duty : 0;
	d->fault = pins[pin].fault;
	d->mismatch_cnt = pins[pin].mismatch;
	spin_unlock_irqrestore(&gpio_lock, flags);
	return 0;
}

static int do_clear_fault(u32 pin)
{
	unsigned long flags;

	if (!valid_pin(pin))
		return -EINVAL;
	spin_lock_irqsave(&gpio_lock, flags);
	pins[pin].fault = 0;
	pins[pin].mismatch = 0;
	spin_unlock_irqrestore(&gpio_lock, flags);
	return 0;
}

/* ---------- file operations ---------- */
static int deviceFile_open(struct inode *inode, struct file *filp)
{
	atomic_inc(&open_cnt);
	return 0;
}

static int deviceFile_release(struct inode *inode, struct file *filp)
{
	/* 마지막 사용자가 닫히면(정상 종료, Ctrl+C, 비정상 종료 모두) 출력 OFF */
	if (atomic_dec_and_test(&open_cnt))
		failsafe_all_off("device closed");
	return 0;
}

static long deviceFile_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ssafy_gpio_req req;
	struct ssafy_gpio_diag diag;
	int ret;

	switch (cmd) {
	case SSAFY_IOC_PINMODE:
	case SSAFY_IOC_WRITE:
	case SSAFY_IOC_READ:
	case SSAFY_IOC_PWM:
	case SSAFY_IOC_CLEAR_FAULT:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		break;
	case SSAFY_IOC_DIAG:
		if (copy_from_user(&diag, (void __user *)arg, sizeof(diag)))
			return -EFAULT;
		ret = do_diag(&diag);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &diag, sizeof(diag)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}

	switch (cmd) {
	case SSAFY_IOC_PINMODE:     return do_pinmode(req.pin, req.value);
	case SSAFY_IOC_WRITE:       return do_write(req.pin, req.value);
	case SSAFY_IOC_PWM:         return do_pwm(req.pin, req.value);
	case SSAFY_IOC_CLEAR_FAULT: return do_clear_fault(req.pin);
	case SSAFY_IOC_READ:
		ret = do_read(req.pin, &req.value);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations fops = {
	.owner          = THIS_MODULE,
	.open           = deviceFile_open,
	.release        = deviceFile_release,
	.unlocked_ioctl = deviceFile_ioctl,
};

/* ---------- init / exit ---------- */
static int __init ssafy_gpio_init(void)
{
	struct device *d;

	peri_base = ioremap(RP1_PERI_BASE, RP1_MAP_SIZE);
	if (!peri_base)
		return -ENOMEM;
	io_bank0   = peri_base + IO_BANK0_OFS;
	pads_bank0 = peri_base + PADS_BANK0_OFS;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&pwm_timer, pwm_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pwm_timer.function = pwm_cb;
#endif

	NOD_MAJOR = register_chrdev(0, SSAFY_GPIO_DEV_NAME, &fops);
	if (NOD_MAJOR < 0) {
		iounmap(peri_base);
		return NOD_MAJOR;
	}
	dev = MKDEV(NOD_MAJOR, 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	cls = class_create(SSAFY_GPIO_DEV_NAME);
#else
	cls = class_create(THIS_MODULE, SSAFY_GPIO_DEV_NAME);
#endif
	if (IS_ERR(cls)) {
		unregister_chrdev(NOD_MAJOR, SSAFY_GPIO_DEV_NAME);
		iounmap(peri_base);
		return PTR_ERR(cls);
	}
	d = device_create(cls, NULL, dev, NULL, SSAFY_GPIO_DEV_NAME);
	if (IS_ERR(d)) {
		class_destroy(cls);
		unregister_chrdev(NOD_MAJOR, SSAFY_GPIO_DEV_NAME);
		iounmap(peri_base);
		return PTR_ERR(d);
	}

	pr_info("ssafy_gpio: loaded, /dev/%s major %d\n", SSAFY_GPIO_DEV_NAME, NOD_MAJOR);
	return 0;
}

static void __exit ssafy_gpio_exit(void)
{
	failsafe_all_off("rmmod");
	device_destroy(cls, dev);
	class_destroy(cls);
	unregister_chrdev(NOD_MAJOR, SSAFY_GPIO_DEV_NAME);
	iounmap(peri_base);
	pr_info("ssafy_gpio: unloaded\n");
}

module_init(ssafy_gpio_init);
module_exit(ssafy_gpio_exit);
