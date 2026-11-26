KERNEL_HEADERS=/lib/modules/$(shell uname -r)/build
CC = gcc
CFLAGS = -Wall -O2

TARGET := switch_app api_test
obj-m += ssafy_gpio_drv.o

PWD := $(CURDIR)

#make 시 make all 실행
all: driver app

#driver build
driver:
	make -C $(KERNEL_HEADERS) M=$(PWD) modules

#app build (미들웨어 ssafy_gpio.c 같이 링크)
app: switch_app api_test

switch_app: switch_app.c ssafy_gpio.c ssafy_gpio.h ssafy_gpio_ioctl.h
	$(CC) $(CFLAGS) -o $@ switch_app.c ssafy_gpio.c

api_test: api_test.c ssafy_gpio.c ssafy_gpio.h ssafy_gpio_ioctl.h
	$(CC) $(CFLAGS) -o $@ api_test.c ssafy_gpio.c

#driver, app 모두 제거
clean:
	make -C $(KERNEL_HEADERS) M=$(PWD) clean
	rm -f *.o $(TARGET)
